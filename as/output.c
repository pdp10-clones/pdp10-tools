/*
 * output.c
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pdp10-elf36.h"
#include "pdp10-stdint.h"
#include "pdp10-stdio.h"
#include "assemble.h"
#include "hashtab.h"
#include "output.h"

struct strtab_entry {
    struct strtab_entry *next;
    const char *string;
    unsigned int nrbytes;	/* strlen(string) + 1 */
};

struct strtab {
    struct strtab_entry *head;
    unsigned int nrbytes;
};

static void strtab_init(struct strtab *strtab)
{
    strtab->head = NULL;
    strtab->nrbytes = 0;
}

static pdp10_uint36_t strtab_enter(struct tunit *tunit, struct strtab *strtab, const char *name)
{
    struct strtab_entry *prev, *here;
    pdp10_uint36_t index;

    index = 1;
    prev = NULL;
    here = strtab->head;
    while (here != NULL) {
	if (strcmp(name, here->string) == 0)
	    return index;
	index += here->nrbytes;
	prev = here;
	here = here->next;
    }

    here = malloc(sizeof *here);
    if (!here) {
	fprintf(stderr, "%s: failed to allocate %zu bytes for a strtab_entry: %s\n",
		tunit->progname, sizeof *here, strerror(errno));
	return 0;
    }
    here->next = NULL;
    here->string = name;
    here->nrbytes = strlen(name) + 1;

    if (prev) {
	prev->next = here;
    } else {
	strtab->head = here;
	index = 1;
	strtab->nrbytes = 1;
    }

    strtab->nrbytes += here->nrbytes;

    return index;
}

static int output_padding(PDP10_FILE *pdp10fp, unsigned int nrbytes)
{
    for (; nrbytes; --nrbytes)
	if (pdp10_elf36_write_uint9(pdp10fp, '\0') < 0)
	    return -1;
    return 0;
}

static int strtab_write(PDP10_FILE *pdp10fp, const struct strtab *strtab)
{
    struct strtab_entry *here;
    unsigned int i;

    if (pdp10_elf36_write_uint9(pdp10fp, '\0') < 0)
	return -1;

    for (here = strtab->head; here; here = here->next)
	for (i = 0; i < here->nrbytes; ++i)
	    if (pdp10_elf36_write_uint9(pdp10fp, here->string[i]) < 0)
		return -1;

    return 0;
}

struct context {
    struct tunit *tunit;
    Elf36_Word shnum;
    Elf36_Word offset;
    struct strtab shstrtab;
    Elf36_Word symnum;
    struct strtab symstrtab;
    PDP10_FILE *pdp10fp;
};

static int append_section(struct context *context, struct section *section)
{
    if (section->dot == 0 || section->image_words == NULL)
	return 0;

    section->st_shndx = context->shnum;
    ++context->shnum;

    section->sh_offset = (context->offset + (section->sh_addralign - 1)) & ~(section->sh_addralign - 1);
    context->offset = section->sh_offset + section->dot;

    section->sh_name = strtab_enter(context->tunit, &context->shstrtab, section->name);
    if (section->sh_name == 0)
	return -1;

    return 0;
}

static int process_section(struct hashnode *hashnode, void *data)
{
    struct section *section = (struct section*)hashnode;
    struct context *context = data;

    return append_section(context, section);
}

static int output_section_prologue(struct context *context, struct section *section)
{
    if (section->st_shndx != context->shnum)
	abort();
    ++context->shnum;

    if (section->sh_offset < context->offset)
	abort();

    if (output_padding(context->pdp10fp, section->sh_offset - context->offset) < 0)
	return -1;

    return 0;
}

static void output_section_epilogue(struct context *context, struct section *section)
{
    context->offset = section->sh_offset + section->dot;
}

static int output_section(struct hashnode *hashnode, void *data)
{
    struct section *section = (struct section*)hashnode;
    struct context *context = data;
    unsigned int i;

    if (section->dot == 0 || section->image_words == NULL)
	return 0;

    if (output_section_prologue(context, section) < 0)
	return -1;

    /* XXX: ->image_words[] should be uint9_t[] not uint36_t[] */
    for (i = 0; i < section->dot; i += 4)
	if (pdp10_elf36_write_uint36(context->pdp10fp, section->image_words[i / 4]) < 0)
	    return -1;

    output_section_epilogue(context, section);

    return 0;
}

static int output_section_header(struct context *context, const struct section *section)
{
    Elf36_Shdr shdr;

    shdr.sh_name = section->sh_name;
    shdr.sh_type = section->sh_type;
    shdr.sh_flags = section->sh_flags;
    shdr.sh_addr = 0;
    shdr.sh_offset = section->sh_offset;
    shdr.sh_size = section->dot;
    shdr.sh_link = section->sh_link;
    shdr.sh_info = 0; /* XXX: for symtab, LAST_LOCAL + 1 */
    shdr.sh_addralign = section->sh_addralign;
    shdr.sh_entsize = section->sh_entsize;

    return pdp10_elf36_write_shdr(context->pdp10fp, &shdr);
}

static int output_shdr(struct hashnode *hashnode, void *data)
{
    struct section *section = (struct section*)hashnode;
    struct context *context = data;


    if (section->dot == 0 || section->image_words == NULL)
	return 0;

    return output_section_header(context, section);
}

static int output_strtab(struct context *context, struct section *section, struct strtab *strtab)
{
    if (output_section_prologue(context, section) < 0)
	return -1;
    if (strtab_write(context->pdp10fp, strtab) < 0)
	return -1;
    output_section_epilogue(context, section);
    return 0;
}

static int process_symbol(struct hashnode *hashnode, void *data)
{
    struct symbol *symbol = (struct symbol*)hashnode;
    struct context *context = data;

    ++context->symnum;

    symbol->st_name = strtab_enter(context->tunit, &context->symstrtab, symbol->name);
    if (symbol->st_name == 0)
	return -1;

    return 0;
}

struct finalize_symbol_context {
    Elf36_Sym *symtab;
    unsigned int i;
};

static int finalize_symbol(struct hashnode *hashnode, void *data)
{
    struct symbol *symbol = (struct symbol*)hashnode;
    struct finalize_symbol_context *fsctx = data;

    fsctx->symtab[fsctx->i].st_name = symbol->st_name;
    fsctx->symtab[fsctx->i].st_value = symbol->st_value;
    fsctx->symtab[fsctx->i].st_size = symbol->st_size;
    fsctx->symtab[fsctx->i].st_info = symbol->st_info;
    fsctx->symtab[fsctx->i].st_other = STV_DEFAULT;
    fsctx->symtab[fsctx->i].st_shndx = symbol->section->st_shndx;

    ++fsctx->i;

    return 0;
}

int output(struct tunit *tunit, const char *outfile)
{
    struct context context;
    struct section section_symtab;
    struct section section_strtab;
    struct section section_shstrtab;
    Elf36_Ehdr ehdr;

    context.tunit = tunit;
    context.shnum = 1;
    context.offset = ELF36_EHDR_SIZEOF;
    strtab_init(&context.shstrtab);
    context.symnum = 0;
    strtab_init(&context.symstrtab);
    context.pdp10fp = NULL;

    if (hashtab_enumerate(&tunit->sections, process_section, &context) < 0)
	return -1;

    if (hashtab_enumerate(&tunit->symbols, process_symbol, &context) < 0)
	return -1;

    section_init(&section_symtab, ".symtab");
    section_init(&section_strtab, ".strtab");
    section_init(&section_shstrtab, ".shstrtab");

    /* if we have symbols, synthesize .strtab and .symtab */
    if (context.symnum) {
	Elf36_Sym *symtab;
	struct finalize_symbol_context fsctx;

	section_strtab.sh_type = SHT_STRTAB;
	section_strtab.sh_addralign = 1;
	section_strtab.dot = context.symstrtab.nrbytes; /* XXX: fixme */
	section_strtab.image_words = (pdp10_uint36_t*)4; /* XXX: fixme */

	if (append_section(&context, &section_strtab) < 0)
	    return -1;

	++context.symnum;	/* for initial stub entry */

	symtab = malloc(context.symnum * sizeof(Elf36_Sym));
	if (!symtab) {
	    fprintf(stderr, "%s: failed to allocate %zu bytes for Elf36 symbol table: %s\n",
		    tunit->progname, context.symnum * sizeof(Elf36_Sym), strerror(errno));
	    return -1;
	}

	symtab[0].st_name = 0;
	symtab[0].st_value = 0;
	symtab[0].st_size = 0;
	symtab[0].st_info = ELF36_ST_INFO(STB_LOCAL, STT_NOTYPE);
	symtab[0].st_other = 0;
	symtab[0].st_shndx = SHN_UNDEF;

	fsctx.symtab = symtab;
	fsctx.i = 1;

	if (hashtab_enumerate(&tunit->symbols, finalize_symbol, &fsctx) < 0)
	    return -1;

	section_symtab.sh_type = SHT_SYMTAB;
	section_symtab.sh_entsize = ELF36_SYM_SIZEOF;
	section_symtab.sh_link = section_strtab.st_shndx;
	section_symtab.sh_addralign = 4;	/* XXX: PDP10-specific */
	section_symtab.dot = context.symnum * ELF36_SYM_SIZEOF; /* XXX: fixme */
	section_symtab.image_words = (pdp10_uint36_t*)symtab; /* XXX: fixme */

	if (append_section(&context, &section_symtab) < 0)
	    return -1;
    }

    /* if we have sections, synthesize .shstrtab */
    if (context.shnum > 1) {
	section_shstrtab.sh_type = SHT_STRTAB;
	section_shstrtab.sh_addralign = 1;

	/* append_section() open-coded and rearranged to work for this special case */

	section_shstrtab.sh_name = strtab_enter(tunit, &context.shstrtab, ".shstrtab");
	if (section_shstrtab.sh_name == 0)
	    return -1;

	section_shstrtab.dot = context.shstrtab.nrbytes; /* XXX: fixme */
	section_shstrtab.image_words = (pdp10_uint36_t*)4; /* XXX: fixme */

	section_shstrtab.st_shndx = context.shnum;
	++context.shnum;

	section_shstrtab.sh_offset = context.offset;
	context.offset = section_shstrtab.sh_offset + section_shstrtab.dot;

	context.offset = (context.offset + (4 - 1)) & ~(Elf36_Word)(4 - 1);

	/* context.offset is now the offset of the section header table, which is last in the file */
    } else {
	context.shnum = 0;
	context.offset = 0;
    }

    ehdr.e_ident[EI_MAG0] = ELFMAG0;
    ehdr.e_ident[EI_MAG1] = ELFMAG1;
    ehdr.e_ident[EI_MAG2] = ELFMAG2;
    ehdr.e_ident[EI_MAG3] = ELFMAG3;
    ehdr.e_ident[EI_CLASS] = ELFCLASS36;
    ehdr.e_ident[EI_DATA] = ELFDATA2MSB;
    ehdr.e_ident[EI_VERSION] = EV_CURRENT;
    ehdr.e_ident[EI_OSABI] = ELFOSABI_NONE;
    ehdr.e_ident[EI_ABIVERSION] = 0;
    {
	int i;

	for (i = EI_PAD; i < EI_NIDENT; ++i)
	    ehdr.e_ident[i] = 0;
    }
    ehdr.e_type = ET_REL;
    ehdr.e_machine = EM_PDP10;
    ehdr.e_version = EV_CURRENT;
    ehdr.e_entry = 0;
    ehdr.e_phoff = 0;
    ehdr.e_shoff = context.offset;
    ehdr.e_flags = 0;
    ehdr.e_ehsize = ELF36_EHDR_SIZEOF;
    ehdr.e_phentsize = 0;
    ehdr.e_phnum = 0;
    ehdr.e_shentsize = ELF36_SHDR_SIZEOF;
    ehdr.e_shnum = context.shnum;
    ehdr.e_shstrndx = section_shstrtab.st_shndx;

    context.pdp10fp = pdp10_fopen(outfile, "wb");
    if (!context.pdp10fp) {
	fprintf(stderr, "%s: failed to open %s: %s\n", tunit->progname, outfile, strerror(errno));
	return -1;
    }

    if (pdp10_elf36_write_ehdr(context.pdp10fp, &ehdr) < 0)
	return -1;

    context.shnum = 1;
    context.offset = ELF36_EHDR_SIZEOF;

    if (hashtab_enumerate(&tunit->sections, output_section, &context) < 0)
	return -1;

    if (context.symnum) {
	unsigned int i;

	if (output_strtab(&context, &section_strtab, &context.symstrtab) < 0)
	    return -1;

	if (output_section_prologue(&context, &section_symtab) < 0)
	    return -1;

	for (i = 0; i < context.symnum; ++i)
	    if (pdp10_elf36_write_sym(context.pdp10fp,
				      &((Elf36_Sym*)section_symtab.image_words)[i]) < 0)
		return -1;

	output_section_epilogue(&context, &section_symtab);
    }

    if (context.shnum > 1) {
	struct section section0;

	if (output_strtab(&context, &section_shstrtab, &context.shstrtab) < 0)
	    return -1;

	if (ehdr.e_shoff < context.offset)
	    abort();
	if (output_padding(context.pdp10fp, ehdr.e_shoff - context.offset) < 0)
	    return -1;
	section0.name = "<fake section 0>";
	section0.sh_name = 0;
	section0.sh_type = SHT_NULL;
	section0.sh_flags = 0;
	section0.sh_offset = 0;
	section0.dot = 0;
	section0.sh_link = 0;
	section0.sh_addralign = 0;
	section0.sh_entsize = 0;
	if (output_section_header(&context, &section0) < 0)
	    return -1;
	if (hashtab_enumerate(&tunit->sections, output_shdr, &context) < 0)
	    return -1;
	if (context.symnum) {
	    if (output_section_header(&context, &section_strtab) < 0)
		return -1;
	    if (output_section_header(&context, &section_symtab) < 0)
		return -1;
	}
	if (output_section_header(&context, &section_shstrtab) < 0)
	    return -1;
    }

    pdp10_fclose(context.pdp10fp);

    return 0;
}
