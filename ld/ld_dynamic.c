/*-
 * Copyright (c) 2012 Kai Wang
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "ld.h"
#include "ld_arch.h"
#include "ld_dynamic.h"
#include "ld_file.h"
#include "ld_hash.h"
#include "ld_input.h"
#include "ld_layout.h"
#include "ld_output.h"
#include "ld_symbols.h"
#include "ld_symver.h"
#include "ld_strtab.h"

ELFTC_VCSID("$Id$");

static void _create_dynamic(struct ld *ld);
static void _create_interp(struct ld *ld);
static void _create_dynsym_and_dynstr(struct ld *ld);
static void _finalize_dynamic(struct ld *ld);

void
ld_dynamic_create(struct ld *ld)
{
	struct ld_input *li;
	struct ld_output *lo;

	lo = ld->ld_output;
	assert(lo != NULL);

	/* Check how many DSOs is needed for output object. */
	lo->lo_dso_needed = 0;
	STAILQ_FOREACH(li, &ld->ld_lilist, li_next) {
		if (li->li_type != LIT_DSO)
			continue;
		if (li->li_dso_refcnt > 0 || !li->li_file->lf_as_needed)
			lo->lo_dso_needed++;
	}

	/* Link statically if we don't use DSOs? */
	if (lo->lo_dso_needed == 0)
		return;

	/* Create .interp section. */
	_create_interp(ld);

	/* Create PLT and GOT sections. */
	ld->ld_arch->create_pltgot(ld);

	/* Create .dynamic section. */
	_create_dynamic(ld);

	/* Copy relevant symbols to internal dynsym table. */
	ld_symbols_create_dynsym(ld);

	/* Create .dynsym and .dynstr sections. */
	_create_dynsym_and_dynstr(ld);

	/* Create .hash section. */
	ld_hash_create_svr4_hash_section(ld);

	/* Create .gnu.version_r section. */
	ld_symver_create_verneed_section(ld);

	/* Create .gnu.version section. */
	ld_symver_create_versym_section(ld);
}

void
ld_dynamic_finalize(struct ld *ld)
{
	struct ld_output *lo;

	lo = ld->ld_output;
	assert(lo != NULL);

	if (lo->lo_dso_needed == 0)
		return;

	/* Finalize PLT and GOT sections. */
	ld->ld_arch->finalize_pltgot(ld);

	/* Finalize .dynamic section */
	_finalize_dynamic(ld);
}

static void
_create_interp(struct ld *ld)
{
	struct ld_output *lo;
	struct ld_output_section *os;
	struct ld_output_data_buffer *odb;
	const char *interp;
	char interp_name[] = ".interp";

	lo = ld->ld_output;
	assert(lo != NULL);

	HASH_FIND_STR(lo->lo_ostbl, interp_name, os);
	if (os == NULL)
		os = ld_layout_insert_output_section(ld, interp_name,
		    SHF_ALLOC);
	os->os_type = SHT_PROGBITS;
	os->os_align = 1;
	os->os_entsize = 0;
	os->os_flags = SHF_ALLOC;

	lo->lo_interp = os;

	if (ld->ld_interp != NULL)
		interp = ld->ld_interp;
	else
		interp = ld->ld_arch->interp;
	assert(interp != NULL);

	if ((odb = calloc(1, sizeof(*odb))) == NULL)
		ld_fatal_std(ld, "calloc");
	odb->odb_size = strlen(interp) + 1;
	odb->odb_align = 1;
	odb->odb_type = ELF_T_BYTE;

	if ((odb->odb_buf = calloc(odb->odb_size, 1)) == NULL)
		ld_fatal_std(ld, "calloc");
	strncpy(odb->odb_buf, interp, strlen(interp));
	odb->odb_buf[strlen(interp)] = '\0';

	(void) ld_output_create_element(ld, &os->os_e, OET_DATA_BUFFER, odb,
	    NULL);
}

static void
_create_dynamic(struct ld *ld)
{
	struct ld_output *lo;
	struct ld_output_section *os, *_os;
	struct ld_output_data_buffer *odb;
	char dynamic_name[] = ".dynamic";
	char init_name[] = ".init";
	char fini_name[] = ".fini";
	int entries;

	lo = ld->ld_output;
	assert(lo != NULL);

	HASH_FIND_STR(lo->lo_ostbl, dynamic_name, os);
	if (os == NULL)
		os = ld_layout_insert_output_section(ld, dynamic_name,
		    SHF_ALLOC | SHF_WRITE);
	os->os_type = SHT_PROGBITS;
	os->os_flags = SHF_ALLOC | SHF_WRITE;
	if (lo->lo_ec == ELFCLASS32) {
		os->os_entsize = 8;
		os->os_align = 4;
	} else {
		os->os_entsize = 16;
		os->os_align = 8;
	}

	entries = lo->lo_dso_needed;

	/* DT_INIT */
	HASH_FIND_STR(lo->lo_ostbl, init_name, _os);
	if (_os != NULL && !os->os_empty) {
		lo->lo_init = _os;
		entries++;
	}

	/* DT_FINI */
	HASH_FIND_STR(lo->lo_ostbl, fini_name, _os);
	if (_os != NULL && !os->os_empty) {
		lo->lo_fini = _os;
		entries++;
	}

	/* DT_HASH, DT_STRTAB, DT_SYMTAB, DT_STRSZ and DT_SYMENT */
	if (HASH_CNT(hhimp, ld->ld_symtab_import) > 0 ||
	    HASH_CNT(hhexp, ld->ld_symtab_export) > 0)
		entries += 5;

	/* TODO: DT_RPATH. */

	/*
	 * DT_DEBUG. dynamic linker changes this at runtime, gdb uses
	 * it to find all the loaded DSOs. (thus .dynamic has to be
	 * writable)
	 */
	entries++;

	/* TODO: DT_PLTGOT, DT_PLTRELSZ, DT_PLTREL and DT_JMPREL. */
	entries += 4;

	/*
	 * TODO: DT_VERNEED, DT_VERNEEDNUM, DT_VERDEF, DT_VERDEFNUM and
	 * DT_VERSYM.
	 */
	entries += 5;

	/* DT_NULL. TODO: Reserve multiple DT_NULL entries for DT_RPATH? */
	entries++;

	/*
	 * Reserve space for .dynamic section, based on number of entries.
	 */
	if ((odb = calloc(1, sizeof(*odb))) == NULL)
		ld_fatal_std(ld, "calloc");
	odb->odb_size = entries * os->os_entsize;
	if ((odb->odb_buf = malloc(odb->odb_size)) == NULL)
		ld_fatal_std(ld, "malloc");
	odb->odb_align = os->os_align;
	odb->odb_type = ELF_T_DYN;

	(void) ld_output_create_element(ld, &os->os_e, OET_DATA_BUFFER, odb,
	    NULL);

	lo->lo_dynamic_odb = odb;

	/* Create _DYNAMIC symobl. */
	ld_symbols_add_internal(ld, "_DYNAMIC", 0, 0, SHN_ABS, STB_LOCAL,
	    STT_OBJECT, STV_HIDDEN, os);
}

#define	DT_ENTRY_VAL(tag,val)					\
	do {							\
		if (lo->lo_ec == ELFCLASS32) {			\
			assert(dt32 < end32); 			\
			dt32->d_tag = (int32_t) (tag);		\
			dt32->d_un.d_val = (uint32_t) (val);	\
			dt32++;					\
		} else {					\
			assert(dt64 < end64); 			\
			dt64->d_tag = (tag);			\
			dt64->d_un.d_val = (val);		\
			dt64++;					\
		}						\
	} while(0)

#define DT_ENTRY_PTR(tag,ptr)					\
	do {							\
		if (lo->lo_ec == ELFCLASS32) {			\
			assert(dt32 < end32); 			\
			dt32->d_tag = (int32_t) (tag);		\
			dt32->d_un.d_ptr = (uint32_t) (ptr);	\
			dt32++;					\
		} else {					\
			assert(dt64 < end64); 			\
			dt64->d_tag = (tag);			\
			dt64->d_un.d_ptr = (ptr);		\
			dt64++;					\
		}						\
	} while(0)

#define DT_ENTRY_NULL						\
	do {							\
		if (lo->lo_ec == ELFCLASS32) {			\
			assert(dt32 < end32); 			\
			while (dt32 < end32)			\
				DT_ENTRY_VAL(DT_NULL, 0);	\
			assert(dt32 == end32);			\
		} else {					\
			assert(dt64 < end64); 			\
			while (dt64 < end64)			\
				DT_ENTRY_VAL(DT_NULL, 0);	\
			assert(dt64 == end64);			\
		}						\
	} while(0)

static void
_finalize_dynamic(struct ld *ld)
{
	struct ld_output *lo;
	struct ld_output_data_buffer *odb;
	Elf32_Dyn *dt32, *end32;
	Elf64_Dyn *dt64, *end64;

	lo = ld->ld_output;
	assert(lo != NULL);

	odb = lo->lo_dynamic_odb;
	assert(odb != NULL);

	dt32 = (Elf32_Dyn *) (uintptr_t) odb->odb_buf;
	dt64 = (Elf64_Dyn *) (uintptr_t) odb->odb_buf;
	end32 = (Elf32_Dyn *) (uintptr_t) (odb->odb_buf + odb->odb_size);
	end64 = (Elf64_Dyn *) (uintptr_t) (odb->odb_buf + odb->odb_size);

	/* TODO: DT_NEEDED. */

	/* DT_INIT and DT_FINI */
	if (lo->lo_init != NULL)
		DT_ENTRY_PTR(DT_INIT, lo->lo_init->os_addr);
	if (lo->lo_fini != NULL)
		DT_ENTRY_PTR(DT_FINI, lo->lo_fini->os_addr);

	/* DT_HASH */
	if (lo->lo_hash != NULL)
		DT_ENTRY_PTR(DT_HASH, lo->lo_hash->os_addr);

	/* DT_HASH, DT_STRTAB, DT_SYMTAB, DT_STRSZ and DT_SYMENT */
	if (lo->lo_dynsym != NULL && lo->lo_dynstr != NULL) {
		DT_ENTRY_PTR(DT_STRTAB, lo->lo_dynstr->os_addr);
		DT_ENTRY_PTR(DT_SYMTAB, lo->lo_dynsym->os_addr);
		DT_ENTRY_VAL(DT_STRSZ, ld->ld_dynstr->st_size);
		DT_ENTRY_VAL(DT_SYMENT, ld->ld_dynsym->sy_size);
	}

	/* DT_DEBUG */
	DT_ENTRY_VAL(DT_DEBUG, 0);

	/* TODO: DT_PLTGOT, DT_PLTRELSZ, DT_PLTREL and DT_JMPREL. */
	if (lo->lo_got != NULL)
		DT_ENTRY_PTR(DT_PLTGOT, lo->lo_got->os_addr);

	/*
	 * TODO: DT_VERNEED, DT_VERNEEDNUM, DT_VERDEF, DT_VERDEFNUM and
	 * DT_VERSYM.
	 */
	if (lo->lo_verneed != NULL) {
		DT_ENTRY_PTR(DT_VERNEED, lo->lo_verneed->os_addr);
		DT_ENTRY_PTR(DT_VERNEEDNUM, lo->lo_verneed_num);
	}
	if (lo->lo_versym != NULL)
		DT_ENTRY_PTR(DT_VERSYM, lo->lo_versym->os_addr);

	/* Fill in the space left with DT_NULL entries */
	DT_ENTRY_NULL;
}

static void
_create_dynsym_and_dynstr(struct ld *ld)
{
	struct ld_output *lo;
	struct ld_output_section *os;
	char dynsym_name[] = ".dynsym";
	char dynstr_name[] = ".dynstr";

	lo = ld->ld_output;
	assert(lo != NULL);

	/*
	 * Create .dynsym section.
	 */

	HASH_FIND_STR(lo->lo_ostbl, dynsym_name, os);
	if (os == NULL)
		os = ld_layout_insert_output_section(ld, dynsym_name,
		    SHF_ALLOC);
	os->os_type = SHT_DYNSYM;
	os->os_flags = SHF_ALLOC;
	if (lo->lo_ec == ELFCLASS32) {
		os->os_entsize = sizeof(Elf32_Sym);
		os->os_align = 4;
	} else {
		os->os_entsize = sizeof(Elf64_Sym);
		os->os_align = 8;
	}
	lo->lo_dynsym = os;

	(void) ld_output_create_element(ld, &os->os_e, OET_SYMTAB,
	    ld->ld_dynsym, NULL);

	/*
	 * Create .dynstr section.
	 */

	HASH_FIND_STR(lo->lo_ostbl, dynstr_name, os);
	if (os == NULL)
		os = ld_layout_insert_output_section(ld, dynstr_name,
		    SHF_ALLOC);
	os->os_type = SHT_STRTAB;
	os->os_flags = SHF_ALLOC;
	os->os_entsize = 0;
	os->os_align = 1;
	lo->lo_dynstr = os;

	(void) ld_output_create_element(ld, &os->os_e, OET_STRTAB,
	    ld->ld_dynstr, NULL);

	lo->lo_dynsym->os_link = os;
}
