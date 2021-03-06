/*
 * input.c
 * Copyright (C) 2013-2015  Mikael Pettersson
 *
 * This file is part of pdp10-tools.
 *
 * pdp10-tools is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * pdp10-tools is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with pdp10-tools.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hashtab.h"
#include "input.h"
#include "parse.h"
#include "scan.h"
#include "tunit.h"

static int do_append(struct scan_state *scan_state, struct tunit *tunit, struct stmt *stmt, unsigned long dot_incr)
{
    struct stmt *stmt2;
    struct section *section;

    stmt2 = malloc(sizeof *stmt2);
    if (!stmt2) {
	fprintf(stderr, "%s: %s line %u: malloc(%zu) failed: %s\n",
		scan_state->progname, scan_state->filename, scan_state->linenr, sizeof *stmt2, strerror(errno));
	return -1;
    }

    *stmt2 = *stmt;
    stmt2->next = NULL;

    section = tunit->cursect;

    *section->tailptr = stmt2;
    section->tailptr = &stmt2->next;
    section->dot += dot_incr;

    return 0;
}

static int do_dot_file(struct scan_state *scan_state, struct tunit *tunit, struct stmt *stmt)
{
    struct symbol *symbol;

    symbol = tunit_symbol_enter(tunit, stmt->u.string.text);
    if (!symbol)
	return -1;

    symbol->section = NULL;
    symbol->defined = 1;
    symbol->st_value = 0;
    symbol->st_size = 0;
    symbol->st_info = ELF_ST_INFO(STB_LOCAL, STT_FILE);

    return 0;
}

static int do_dot_globl(struct scan_state *scan_state, struct tunit *tunit, struct stmt *stmt)
{
    struct symbol *symbol;

    symbol = tunit_symbol_enter(tunit, stmt->u.symbol.name);
    if (!symbol)
	return -1;

    if (ELF_ST_BIND(symbol->st_info) != STB_LOCAL) {
	fprintf(stderr, "%s: %s line %u: symbol %s already has non-zero binding type %u\n",
		scan_state->progname, scan_state->filename, scan_state->linenr, stmt->u.symbol.name, ELF_ST_BIND(symbol->st_info));
	return -1;
    }

    symbol->st_info = ELF_ST_INFO(STB_GLOBAL, ELF_ST_TYPE(symbol->st_info));

    return 0;
}

static int do_dot_ident(struct scan_state *scan_state, struct tunit *tunit, struct stmt *stmt)
{
    struct strtab *strtab;

    strtab = tunit_strtab_section_enter(tunit, ".comment");
    if (!strtab)
	return -1;

    if (strtab->section.sh_type == SHT_NULL) {
	/* The ELF specification doesn't specify attribute values for .comment
	 * sections, apart from sh_type, but GNU binutils formats them as string
	 * tables with the following attribute values.
	 */
	strtab->section.sh_type = SHT_PROGBITS;
	strtab->section.sh_flags = SHF_MERGE | SHF_STRINGS;
	strtab->section.sh_entsize = 1;
	strtab->section.sh_addralign = 1;
    }

    (void)strtab_enter(tunit, strtab, stmt->u.string.text);

    return 0;
}

static int do_dot_size(struct scan_state *scan_state, struct tunit *tunit, struct stmt *stmt)
{
    struct symbol *symbol;

    symbol = tunit_symbol_enter(tunit, stmt->u.symbol.name);
    if (!symbol)
	return -1;

    if (!symbol->section
	|| !symbol->defined) {
	fprintf(stderr, "%s: %s line %u: symbol %s undefined\n",
		scan_state->progname, scan_state->filename, scan_state->linenr, stmt->u.symbol.name);
	return -1;
    }
    if (symbol->st_size) {
	fprintf(stderr, "%s: %s line %u: size of symbol %s already defined\n",
		scan_state->progname, scan_state->filename, scan_state->linenr, stmt->u.symbol.name);
	return -1;
    }
    if (symbol->section != tunit->cursect) {
	fprintf(stderr, "%s: %s line %u: symbol %s is not defined in current section\n",
		scan_state->progname, scan_state->filename, scan_state->linenr, stmt->u.symbol.name);
	return -1;
    }

    symbol->st_size = tunit->cursect->dot - symbol->st_value;

    return 0;
}

static int do_dot_text(struct scan_state *scan_state, struct tunit *tunit, struct stmt *stmt)
{
    struct section *section;

    section = tunit_section_enter(tunit, ".text");
    if (!section)
	return -1;

    if (section->sh_type == SHT_NULL) {
	section->sh_type = SHT_PROGBITS;
	section->sh_flags = SHF_ALLOC | SHF_EXECINSTR;
	section->sh_addralign = 4;	/* XXX: PDP10-specific */
    }

    tunit->cursect = section;

    return 0;
}

static int do_dot_type_function(struct scan_state *scan_state, struct tunit *tunit, struct stmt *stmt)
{
    struct symbol *symbol;

    symbol = tunit_symbol_enter(tunit, stmt->u.symbol.name);
    if (!symbol)
	return -1;

    if (ELF_ST_TYPE(symbol->st_info) != STT_NOTYPE) {
	fprintf(stderr, "%s: %s line %u: symbol %s already has non-zero type %u\n",
		scan_state->progname, scan_state->filename, scan_state->linenr, stmt->u.symbol.name, ELF_ST_TYPE(symbol->st_info));
	return -1;
    }

    symbol->st_info = ELF_ST_INFO(ELF_ST_BIND(symbol->st_info), STT_FUNC);

    return 0;
}

static int do_label(struct scan_state *scan_state, struct tunit *tunit, struct stmt *stmt)
{
    struct symbol *symbol;
    struct section *section;

    symbol = tunit_symbol_enter(tunit, stmt->u.symbol.name);
    if (!symbol)
	return -1;

    if (symbol->section
	|| symbol->defined
	|| symbol->st_value) {
	fprintf(stderr, "%s: %s line %u: symbol %s already defined\n",
		scan_state->progname, scan_state->filename, scan_state->linenr, stmt->u.symbol.name);
	return -1;
    }

    section = tunit->cursect;

    symbol->section = section;
    symbol->defined = 1;
    symbol->st_value = section->dot;

    return do_append(scan_state, tunit, stmt, 0);
}

static int do_insn(struct scan_state *scan_state, struct tunit *tunit, struct stmt *stmt)
{
    if (tunit->cursect->dot & 3) {	/* XXX: PDP10-specific */
	fprintf(stderr, "%s: %s line %u: misaligned instruction\n",
		scan_state->progname, scan_state->filename, scan_state->linenr);
	return -1;
    }
    return do_append(scan_state, tunit, stmt, 4);	/* XXX: PDP10-specific sizeof */
}

static int interpret(struct scan_state *scan_state, struct tunit *tunit, struct stmt *stmt)
{
    switch (stmt->tag) {
    case S_DOT_FILE:
	return do_dot_file(scan_state, tunit, stmt);
    case S_DOT_GLOBL:
	return do_dot_globl(scan_state, tunit, stmt);
    case S_DOT_IDENT:
	return do_dot_ident(scan_state, tunit, stmt);
    case S_DOT_SIZE:
	return do_dot_size(scan_state, tunit, stmt);
    case S_DOT_TEXT:
	return do_dot_text(scan_state, tunit, stmt);
    case S_DOT_TYPE_FUNCTION:
	return do_dot_type_function(scan_state, tunit, stmt);
    case S_LABEL:
	return do_label(scan_state, tunit, stmt);
    case S_INSN:
	return do_insn(scan_state, tunit, stmt);
    default:
	fprintf(stderr, "%s: %s line %u: parser returned unexpected stmt->tag %u\n",
		scan_state->progname, scan_state->filename, scan_state->linenr, stmt->tag);
	return -1;
    }
}

int input(char **files, int nrfiles, struct tunit *tunit)
{
    char fake_file[3];
    char *fake_files[1];
    struct scan_state scan_state;
    int i;
    struct stmt stmt;
    int status;

    if (nrfiles <= 0) {
	fake_file[0] = '-';
	fake_file[1] = '-';
	fake_file[2] = '\0';
	fake_files[0] = fake_file;
	files = fake_files;
	nrfiles = 1;
    }

    scan_init(&scan_state, tunit->progname);

    for (i = 0; i < nrfiles; ++i) {
	if (scan_open(&scan_state, files[i]) < 0)
	    return -1;
	for (;;) {
	    status = parse_stmt(&scan_state, &stmt);
	    if (status < 0)
		return -1;
	    if (status == 0)
		break;
	    if (interpret(&scan_state, tunit, &stmt) < 0)
		return -1;
	}
    }

    return 0;
}
