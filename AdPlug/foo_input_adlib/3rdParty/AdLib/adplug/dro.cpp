/*
 * Adplug - Replayer for many OPL2/OPL3 audio file formats.
 * Copyright (C) 1999 - 2007 Simon Peter, <dn.tlp@gmx.net>, et al.
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * dro.c - DOSBox Raw OPL Player by Sjoerd van der Berg <harekiet@zophar.net>
 *
 * upgraded by matthew gambrell <zeromus@zeromus.org>
 * Refactored to better match dro2.cpp 
 *  by Laurence Dougal Myers <jestarjokin@jestarjokin.net>
 * 
 * NOTES: 3-oct-04: the DRO format is not yet finalized. beware.
 *        10-jun-12: the DRO 1 format is finalized, but capturing is buggy.
 */

/*
 * Copyright (c) 2012 - 2017 Wraithverge <liam82067@yahoo.com>
 * - Added Member pointers.
 * - Fixed incorrect operator.
 * - Finalized support for displaying arbitrary Tag data.
 */

#include <cstring>
#include <stdio.h>

#include "dro.h"

CPlayer *CdroPlayer::factory(Copl *newopl)
{
  return new CdroPlayer(newopl);
}

CdroPlayer::CdroPlayer(Copl *newopl) :
	CPlayer(newopl),
	data(0),
	iInitBlockEnd(0)
{
}

CdroPlayer::~CdroPlayer()
{
	if (this->data) delete[] this->data;
}

bool CdroPlayer::load(const std::string &filename, const CFileProvider &fp)
{
	binistream *f = fp.open(filename);
	if (!f) return false;

	char id[8];
	f->readString(id, 8);
	if (strncmp(id, "DBRAWOPL", 8)) {
		fp.close(f);
		return false;
	}
	const uint32_t fileSize = static_cast<uint32_t>(fp.filesize(f));
	if (fileSize < 0x11) {
		fp.close(f);
		return false;
	}

	const uint32_t headerVal = static_cast<uint32_t>(f->readInt(4));
	uint16_t verMajor = 0xFFFF;
	uint32_t dataOfs = 0;
	uint32_t dataLen = 0;

	// Same detection strategy as libVGM:
	// - v0: no explicit version bytes
	// - v1: version layout is minor,major (32-bit value low16=minor high16=major)
	if (headerVal & 0xFF00FF00) {
		verMajor = 0; // DRO v0
		dataLen = static_cast<uint32_t>(f->readInt(4)); // bytes
		f->ignore(1); // hardware type (ignored by this player)
		dataOfs = 0x11;
	} else if ((headerVal & 0x0000FFFF) == 0) {
		verMajor = static_cast<uint16_t>((headerVal >> 16) & 0xFFFF); // DRO v1 major
		if (verMajor != 1) {
			fp.close(f);
			return false;
		}
		f->ignore(4); // length in ms
		dataLen = static_cast<uint32_t>(f->readInt(4)); // bytes
		f->ignore(4); // hardware type dword
		dataOfs = 0x18;
	} else {
		// Not DRO v0/v1 (likely v2 - handled by Cdro2Player).
		fp.close(f);
		return false;
	}

	if (dataLen < 1 || dataOfs > fileSize || dataLen > fileSize - dataOfs) {
		fp.close(f);
		return false;
	}

	this->iLength = dataLen;
	this->data = new uint8_t[this->iLength];
	f->seek(dataOfs, binio::Set);
	f->readString(reinterpret_cast<char*>(this->data), this->iLength);

	title[0] = 0;
	author[0] = 0;
	desc[0] = 0;
	// Determine init-block end for v1 tolerance logic (01/04 ambiguities).
	this->iInitBlockEnd = 0;
	if (verMajor == 1) {
		unsigned int pos = 0;
		unsigned int selPort = 0;
		unsigned int lastReg = 0;

		// First pass: monotonic initialization register dump.
		while (pos + 1 < this->iLength) {
			unsigned int cmd = this->data[pos];
			if (cmd == 0x02 || cmd == 0x03) {
				selPort = cmd & 0x01;
				pos++;
				continue;
			}

			unsigned int reg = (selPort << 8) | cmd;
			if (reg < lastReg) break;
			lastReg = reg;
			pos += 2;
		}

		// Second pass: continue until first clear delay marker.
		while (pos + 1 < this->iLength) {
			unsigned int cmd = this->data[pos];
			if (cmd == 0x00 || cmd == 0x01) break;
			if (cmd == 0x02 || cmd == 0x03) {
				selPort = cmd & 0x01;
				pos++;
				continue;
			}
			if (cmd == 0x04 && this->data[pos + 1] < 0x08) break;
			pos += 2;
		}

		this->iInitBlockEnd = pos;
	}

	int tagsize = fp.filesize(f) - f->pos();

	if (tagsize >= 3)
	{
		// The arbitrary Tag Data section begins here.
		if ((uint8_t)f->readInt(1) != 0xFF ||
			(uint8_t)f->readInt(1) != 0xFF ||
			(uint8_t)f->readInt(1) != 0x1A)
		{
			// Tag data does not present or truncated.
			goto end_section;
		}

		// "title" is maximum 40 characters long.
		f->readString(title, 40, 0);

		// Skip "author" if Tag marker byte is missing.
		if (f->readInt(1) != 0x1B) {
			f->seek(-1, binio::Add);
			goto desc_section;
		}

		// "author" is maximum 40 characters long.
		f->readString(author, 40, 0);

desc_section:
		// Skip "desc" if Tag marker byte is missing.
		if (f->readInt(1) != 0x1C) {
			goto end_section;
		}

		// "desc" is now maximum 1023 characters long (it was 140).
		f->readString(desc, 1023, 0);
	}

end_section:
	fp.close(f);
	rewind(0);

	return true;
}

bool CdroPlayer::update()
{
	unsigned int iIndex;
	unsigned int iValue;
	while (this->iPos < this->iLength) {
		iIndex = this->data[this->iPos++];

		// Short delay
		if (iIndex == this->iCmdDelayS) {
			if (this->iPos >= this->iLength) return false;
			iValue = this->data[this->iPos++];
			this->iDelay = iValue + 1;
			return true;

		// Long delay
		} else if (iIndex == this->iCmdDelayL) {
			// Tolerate malformed DRO v1 streams where "01" appears as an
			// unescaped write to register 0x01.
			if (this->iPos < this->iInitBlockEnd) {
				if (this->iPos >= this->iLength) return false;
				iValue = this->data[this->iPos++];
				this->opl->write(iIndex, iValue);
				continue;
			}
			if (this->iPos + 1 < this->iLength &&
				!(this->data[this->iPos] & ~0x20) &&
				(this->data[this->iPos + 1] == 0x08 || this->data[this->iPos + 1] >= 0x20)) {
				iValue = this->data[this->iPos++];
				this->opl->write(iIndex, iValue);
				continue;
			}
			if (this->iPos + 1 >= this->iLength) return false;
			iValue = this->data[this->iPos] | (this->data[this->iPos + 1] << 8);
			this->iPos += 2;
			this->iDelay = (iValue + 1);
			return true;

		// Bank switching
		} else if (iIndex == 0x02 || iIndex == 0x03) {
			this->opl->setchip(iIndex - 0x02);

		// Normal write
		} else {
			if (iIndex == 0x04) {
				if (this->iPos >= this->iLength) return false;
				// Tolerate missing escape in init/data split: treat 0x04 XX as a
				// direct write to register 0x04 unless XX is a valid escaped register.
				if (this->data[this->iPos] < 0x08 && this->iPos >= this->iInitBlockEnd) {
					iIndex = this->data[this->iPos++];
				}
			}
			else if (this->iPos >= this->iLength) return false;
			iValue = this->data[this->iPos++];
			this->opl->write(iIndex, iValue);
		}
	}

	// This won't result in endless-play using Adplay, but IMHO that code belongs
	// in Adplay itself, not here.
	return this->iPos < this->iLength;
}

void CdroPlayer::rewind(int subsong)
{
	this->iDelay = 0;
	this->iPos = 0;
	opl->init();

	// DRO v1 assumes all registers are initialized to 0.
	// Registers not initialized to 0 will be corrected
	//  in the data stream.
	int i;
	opl->setchip(0);
	for(i = 0; i < 256; i++) {
		opl->write(i, 0);
	}
	
	opl->setchip(1);
	for(i = 0; i < 256; i++) {
		opl->write(i, 0);
	}

	opl->setchip(0);
}

float CdroPlayer::getrefresh()
{
	if (this->iDelay > 0) return 1000.0 / this->iDelay;
	else return 1000.0;
}
