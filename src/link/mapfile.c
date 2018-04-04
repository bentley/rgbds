/*
 * This file is part of RGBDS.
 *
 * Copyright (c) 1997-2018, Carsten Sorensen and RGBDS contributors.
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "extern/err.h"

#include "link/assign.h"
#include "link/main.h"
#include "link/mylink.h"

#include "safelibc.h"

static int32_t currentbank;
static int32_t sfbank;
static FILE *mf;
static FILE *sf;

void SetMapfileName(char *name)
{
	mf = fopen(name, "w");

	if (mf == NULL)
		err(1, "Cannot open mapfile '%s'", name);
}

void SetSymfileName(char *name)
{
	sf = fopen(name, "w");

	if (sf == NULL)
		err(1, "Cannot open symfile '%s'", name);

	zfprintf(sf, "%s", "; File generated by rgblink\n\n");
}

void CloseMapfile(void)
{
	if (mf) {
		zfclose(mf);
		mf = NULL;
	}
	if (sf) {
		zfclose(sf);
		sf = NULL;
	}
}

void MapfileInitBank(int32_t bank)
{
	if ((bank < 0) || (bank >= BANK_INDEX_MAX))
		errx(1, "%s: Unknown bank %d\n", __func__, bank);

	if (mf) {
		currentbank = bank;
		if (BankIndexIsROM0(bank)) {
			zfprintf(mf, "%s", "ROM Bank #0 (HOME):\n");
		} else if (BankIndexIsROMX(bank)) {
			zfprintf(mf, "ROM Bank #%d:\n",
				 bank - BANK_INDEX_ROMX + 1);
		} else if (BankIndexIsWRAM0(bank)) {
			zfprintf(mf, "%s", "WRAM Bank #0:\n");
		} else if (BankIndexIsWRAMX(bank)) {
			zfprintf(mf, "WRAM Bank #%d:\n",
				 bank - BANK_INDEX_WRAMX + 1);
		} else if (BankIndexIsVRAM(bank)) {
			zfprintf(mf, "VRAM Bank #%d:\n",
				 bank - BANK_INDEX_VRAM);
		} else if (BankIndexIsOAM(bank)) {
			zfprintf(mf, "%s", "OAM:\n");
		} else if (BankIndexIsHRAM(bank)) {
			zfprintf(mf, "%s", "HRAM:\n");
		} else if (BankIndexIsSRAM(bank)) {
			zfprintf(mf, "SRAM Bank #%d:\n",
				 bank - BANK_INDEX_SRAM);
		}
	}
	if (sf) {
		if (BankIndexIsROM0(bank))
			sfbank = 0;
		else if (BankIndexIsROMX(bank))
			sfbank = bank - BANK_INDEX_ROMX + 1;
		else if (BankIndexIsWRAM0(bank))
			sfbank = 0;
		else if (BankIndexIsWRAMX(bank))
			sfbank = bank - BANK_INDEX_WRAMX + 1;
		else if (BankIndexIsVRAM(bank))
			sfbank = bank - BANK_INDEX_VRAM;
		else if (BankIndexIsOAM(bank))
			sfbank = 0;
		else if (BankIndexIsHRAM(bank))
			sfbank = 0;
		else if (BankIndexIsSRAM(bank))
			sfbank = bank - BANK_INDEX_SRAM;
		else
			sfbank = 0;
	}
}

void MapfileWriteSection(const struct sSection *pSect)
{
	int32_t i;

	if (mf) {
		if (pSect->nByteSize > 0) {
			zfprintf(mf, "  SECTION: $%04X-$%04X ($%04X bytes) [\"%s\"]\n",
				 pSect->nOrg,
				 pSect->nOrg + pSect->nByteSize - 1,
				 pSect->nByteSize, pSect->pzName);
		} else {
			zfprintf(mf, "  SECTION: $%04X ($0 bytes) [\"%s\"]\n",
				 pSect->nOrg, pSect->pzName);
		}
	}

	for (i = 0; i < pSect->nNumberOfSymbols; i += 1) {
		const struct sSymbol *pSym = pSect->tSymbols[i];

		/* Don't print '@' */
		if (strcmp(pSym->pzName, "@") == 0)
			continue;

		if ((pSym->pSection == pSect) && (pSym->Type != SYM_IMPORT)) {
			if (mf) {
				zfprintf(mf, "           $%04X = %s\n",
					 pSym->nOffset + pSect->nOrg,
					 pSym->pzName);
			}
			if (sf) {
				zfprintf(sf, "%02X:%04X %s\n", sfbank,
					 pSym->nOffset + pSect->nOrg,
					 pSym->pzName);
			}
		}
	}
}

void MapfileCloseBank(int32_t slack)
{
	if (!mf)
		return;

	if (slack == MaxAvail[currentbank])
		zfprintf(mf, "%s", "  EMPTY\n\n");
	else
		zfprintf(mf, "    SLACK: $%04X bytes\n\n", slack);
}
