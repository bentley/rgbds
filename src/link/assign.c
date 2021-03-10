/*
 * This file is part of RGBDS.
 *
 * Copyright (c) 2019, Eldred Habert and RGBDS contributors.
 *
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "link/assign.h"
#include "link/section.h"
#include "link/symbol.h"
#include "link/object.h"
#include "link/main.h"
#include "link/script.h"
#include "link/output.h"

#include "extern/err.h"
#include "helpers.h"

struct MemoryLocation {
	uint16_t address;
	uint32_t bank;
};

struct FreeSpace {
	uint16_t address;
	uint16_t size;
	struct FreeSpace *next, *prev;
};

/* Table of free space for each bank */
struct FreeSpace *memory[SECTTYPE_INVALID];

uint64_t nbSectionsToAssign;

/**
 * Init the free space-modelling structs
 */
static void initFreeSpace(void)
{
	for (enum SectionType type = 0; type < SECTTYPE_INVALID; type++) {
		memory[type] = malloc(sizeof(*memory[type]) * nbbanks(type));
		if (!memory[type])
			err(1, "Failed to init free space for region %d", type);

		for (uint32_t bank = 0; bank < nbbanks(type); bank++) {
			memory[type][bank].next =
				malloc(sizeof(*memory[type][0].next));
			if (!memory[type][bank].next)
				err(1, "Failed to init free space for region %d bank %" PRIu32,
				    type, bank);
			memory[type][bank].next->address = startaddr[type];
			memory[type][bank].next->size    = maxsize[type];
			memory[type][bank].next->next    = NULL;
			memory[type][bank].next->prev    = &memory[type][bank];
		}
	}
}

/**
 * Alter sections' attributes based on the linker script
 */
static void processLinkerScript(void)
{
	if (!linkerScriptName)
		return;
	verbosePrint("Reading linker script...\n");

	linkerScript = openFile(linkerScriptName, "r");

	/* Modify all sections according to the linker script */
	struct SectionPlacement *placement;

	while ((placement = script_NextSection())) {
		struct Section *section = placement->section;

		/* Check if this doesn't conflict with what the code says */
		if (section->isBankFixed && placement->bank != section->bank)
			error(NULL, 0, "Linker script contradicts \"%s\"'s bank placement",
			      section->name);
		if (section->isAddressFixed && placement->org != section->org)
			error(NULL, 0, "Linker script contradicts \"%s\"'s address placement",
			      section->name);
		if (section->isAlignFixed
		 && (placement->org & section->alignMask) != 0)
			error(NULL, 0, "Linker script contradicts \"%s\"'s alignment",
			      section->name);

		section->isAddressFixed = true;
		section->org = placement->org;
		section->isBankFixed = true;
		section->bank = placement->bank;
		section->isAlignFixed = false; /* The alignment is satisfied */
	}

	fclose(linkerScript);
}

/**
 * Assigns a section to a given memory location
 * @param section The section to assign
 * @param location The location to assign the section to
 */
static inline void assignSection(struct Section *section,
				 struct MemoryLocation const *location)
{
	section->org = location->address;
	section->bank = location->bank;

	nbSectionsToAssign--;

	out_AddSection(section);
}

/**
 * Checks whether a given location is suitable for placing a given section
 * This checks not only that the location has enough room for the section, but
 * also that the constraints (alignment...) are respected.
 * @param section The section to be placed
 * @param freeSpace The candidate free space to place the section into
 * @param location The location to attempt placing the section at
 * @return True if the location is suitable, false otherwise.
 */
static bool isLocationSuitable(struct Section const *section,
			       struct FreeSpace const *freeSpace,
			       struct MemoryLocation const *location)
{
	if (section->isAddressFixed && section->org != location->address)
		return false;

	if (section->isAlignFixed
	 && ((location->address - section->alignOfs) & section->alignMask))
		return false;

	if (location->address < freeSpace->address)
		return false;
	return location->address + section->size
					<= freeSpace->address + freeSpace->size;
}

/**
 * Finds a suitable location to place a section at.
 * @param section The section to be placed
 * @param location A pointer to a location struct that will be filled
 * @return A pointer to the free space encompassing the location, or NULL if
 *         none was found
 */
static struct FreeSpace *getPlacement(struct Section const *section,
				      struct MemoryLocation *location)
{
	location->bank = section->isBankFixed
				? section->bank
				: bankranges[section->type][0];
	struct FreeSpace *space;

	for (;;) {
		/* Switch to the beginning of the next bank */
#define BANK_INDEX (location->bank - bankranges[section->type][0])
		space = memory[section->type][BANK_INDEX].next;
		if (space)
			location->address = space->address;

		/* Process locations in that bank */
		while (space) {
			/* If that location is OK, return it */
			if (isLocationSuitable(section, space, location))
				return space;

			/* Go to the next *possible* location */
			if (section->isAddressFixed) {
				/*
				 * If the address is fixed, there can be only
				 * one candidate block per bank; if we already
				 * reached it, give up.
				 */
				if (location->address < section->org)
					location->address = section->org;
				else
					/* Try again in next bank */
					space = NULL;
			} else if (section->isAlignFixed) {
				/* Move to next aligned location */
				/* Move back to alignment boundary */
				location->address -= section->alignOfs;
				/* Ensure we're there (e.g. on first check) */
				location->address &= ~section->alignMask;
				/* Go to next align boundary and add offset */
				location->address += section->alignMask + 1
							+ section->alignOfs;
			} else {
				/* Any location is fine, so, next free block */
				space = space->next;
				if (space)
					location->address = space->address;
			}

			/*
			 * If that location is past the current block's end,
			 * go forwards until that is no longer the case.
			 */
			while (space && location->address >=
						space->address + space->size)
				space = space->next;

			/* Try again with the new location/free space combo */
		}

		if (section->isBankFixed)
			return NULL;

		/* Try again in the next bank */
		location->bank++;
		if (location->bank > bankranges[section->type][1])
			return NULL;
#undef BANK_INDEX
	}
}

/**
 * Places a section in a suitable location, or error out if it fails to.
 * @warning Due to the implemented algorithm, this should be called with
 *          sections of decreasing size.
 * @param section The section to place
 */
static void placeSection(struct Section *section)
{
	struct MemoryLocation location;

	/* Specially handle 0-byte SECTIONs, as they can't overlap anything */
	if (section->size == 0) {
		/*
		 * Unless the SECTION's address was fixed, the starting address
		 * is fine for any alignment, as checked in sect_DoSanityChecks.
		 */
		location.address = section->isAddressFixed
						? section->org
						: startaddr[section->type];
		location.bank = section->isBankFixed
						? section->bank
						: bankranges[section->type][0];
		assignSection(section, &location);
		return;
	}

	/*
	 * Place section using first-fit decreasing algorithm
	 * https://en.wikipedia.org/wiki/Bin_packing_problem#First-fit_algorithm
	 */
	struct FreeSpace *freeSpace = getPlacement(section, &location);

	if (freeSpace) {
		assignSection(section, &location);

		/* Split the free space */
		bool noLeftSpace  = freeSpace->address == section->org;
		bool noRightSpace = freeSpace->address + freeSpace->size
					== section->org + section->size;
		if (noLeftSpace && noRightSpace) {
			/* The free space is entirely deleted */
			freeSpace->prev->next = freeSpace->next;
			if (freeSpace->next)
				freeSpace->next->prev = freeSpace->prev;
			/*
			 * If the space is the last one on the list, set its
			 * size to 0 so it doesn't get picked, but don't free()
			 * it as it will be freed when cleaning up
			 */
			free(freeSpace);
		} else if (!noLeftSpace && !noRightSpace) {
			/* The free space is split in two */
			struct FreeSpace *newSpace = malloc(sizeof(*newSpace));

			if (!newSpace)
				err(1, "Failed to split new free space");
			/* Append the new space after the chosen one */
			newSpace->prev = freeSpace;
			newSpace->next = freeSpace->next;
			if (freeSpace->next)
				freeSpace->next->prev = newSpace;
			freeSpace->next = newSpace;
			/* Set its parameters */
			newSpace->address = section->org + section->size;
			newSpace->size = freeSpace->address + freeSpace->size -
				newSpace->address;
			/* Set the original space's new parameters */
			freeSpace->size = section->org - freeSpace->address;
			/* address is unmodified */
		} else {
			/* The amount of free spaces doesn't change: resize! */
			freeSpace->size -= section->size;
			if (noLeftSpace)
				/* The free space is moved *and* resized */
				freeSpace->address += section->size;
		}
		return;
	}

	/* Please adjust depending on longest message below */
	char where[64];

	if (section->isBankFixed && nbbanks(section->type) != 1) {
		if (section->isAddressFixed)
			snprintf(where, 64, "at $%02" PRIx32 ":%04" PRIx16,
				 section->bank, section->org);
		else if (section->isAlignFixed)
			snprintf(where, 64, "in bank $%02" PRIx32 " with align mask %" PRIx16,
				 section->bank, (uint16_t)~section->alignMask);
		else
			snprintf(where, 64, "in bank $%02" PRIx32,
				 section->bank);
	} else {
		if (section->isAddressFixed)
			snprintf(where, 64, "at address $%04" PRIx16,
				 section->org);
		else if (section->isAlignFixed)
			snprintf(where, 64, "with align mask %" PRIx16 " and offset %" PRIx16,
				 (uint16_t)~section->alignMask,
				 section->alignOfs);
		else
			strcpy(where, "anywhere");
	}

	/* If a section failed to go to several places, nothing we can report */
	if (!section->isBankFixed || !section->isAddressFixed)
		errx(1, "Unable to place \"%s\" (%s section) %s",
		     section->name, typeNames[section->type], where);
	/* If the section just can't fit the bank, report that */
	else if (section->org + section->size > endaddr(section->type) + 1)
		errx(1, "Unable to place \"%s\" (%s section) %s: section runs past end of region ($%04" PRIx16 " > $%04" PRIx16 ")",
		     section->name, typeNames[section->type], where,
		     section->org + section->size, endaddr(section->type) + 1);
	/* Otherwise there is overlap with another section */
	else
		errx(1, "Unable to place \"%s\" (%s section) %s: section overlaps with \"%s\"",
		     section->name, typeNames[section->type], where,
		     out_OverlappingSection(section)->name);
}

struct UnassignedSection {
	struct Section *section;
	struct UnassignedSection *next;
};

#define  BANK_CONSTRAINED (1 << 2)
#define   ORG_CONSTRAINED (1 << 1)
#define ALIGN_CONSTRAINED (1 << 0)
static struct UnassignedSection *unassignedSections[1 << 3] = {0};
static struct UnassignedSection *sections;

/**
 * Categorize a section depending on how constrained it is
 * This is so the most-constrained sections are placed first
 * @param section The section to categorize
 * @param arg Callback arg, unused
 */
static void categorizeSection(struct Section *section, void *arg)
{
	(void)arg;
	uint8_t constraints = 0;

	if (section->isBankFixed)
		constraints |= BANK_CONSTRAINED;
	if (section->isAddressFixed)
		constraints |= ORG_CONSTRAINED;
	/* Can't have both! */
	else if (section->isAlignFixed)
		constraints |= ALIGN_CONSTRAINED;

	struct UnassignedSection **ptr = &unassignedSections[constraints];

	/* Insert section while keeping the list sorted by decreasing size */
	while (*ptr && (*ptr)->section->size > section->size)
		ptr = &(*ptr)->next;

	sections[nbSectionsToAssign].section = section;
	sections[nbSectionsToAssign].next = *ptr;
	*ptr = &sections[nbSectionsToAssign];

	nbSectionsToAssign++;
}

void assign_AssignSections(void)
{
	verbosePrint("Beginning assignment...\n");

	/** Initialize assignment **/

	/* Generate linked lists of sections to assign */
	sections = malloc(sizeof(*sections) * nbSectionsToAssign + 1);
	if (!sections)
		err(1, "Failed to allocate memory for section assignment");

	initFreeSpace();

	/* Process linker script, if any */
	processLinkerScript();

	/* After the linker script check if we need to do smart linking
	   and discard any sections */
	sect_PerformSmartLink();

	nbSectionsToAssign = 0;
	sect_ForEach(categorizeSection, NULL);

	/** Place sections, starting with the most constrained **/

	/* Specially process fully-constrained sections because of overlaying */
	struct UnassignedSection *sectionPtr =
		unassignedSections[BANK_CONSTRAINED | ORG_CONSTRAINED];

	verbosePrint("Assigning bank+org-constrained...\n");
	while (sectionPtr) {
		placeSection(sectionPtr->section);
		sectionPtr = sectionPtr->next;
	}

	/* If all sections were fully constrained, we have nothing left to do */
	if (!nbSectionsToAssign)
		return;

	/* Overlaying requires only fully-constrained sections */
	verbosePrint("Assigning other sections...\n");
	if (overlayFileName)
		errx(1, "All sections must be fixed when using an overlay file; %" PRIu64 " %sn't",
		     nbSectionsToAssign, nbSectionsToAssign == 1 ? "is" : "are");

	/* Assign all remaining sections by decreasing constraint order */
	for (int8_t constraints = BANK_CONSTRAINED | ALIGN_CONSTRAINED;
	     constraints >= 0; constraints--) {
		sectionPtr = unassignedSections[constraints];

		while (sectionPtr) {
			placeSection(sectionPtr->section);
			sectionPtr = sectionPtr->next;
		}

		if (!nbSectionsToAssign)
			return;
	}

	unreachable_();
}

void assign_Cleanup(void)
{
	for (enum SectionType type = 0; type < SECTTYPE_INVALID; type++) {
		for (uint32_t bank = 0; bank < nbbanks(type); bank++) {
			struct FreeSpace *ptr =
				memory[type][bank].next;

			while (ptr) {
				struct FreeSpace *next = ptr->next;

				free(ptr);
				ptr = next;
			}
		}

		free(memory[type]);
	}

	free(sections);

	script_Cleanup();
}
