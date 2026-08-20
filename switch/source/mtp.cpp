/*
 *   This file is part of Checkpoint
 *   Copyright (C) 2017-2026 Bernardo Giordano, FlagBrew
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *   Additional Terms 7.b and 7.c of GPLv3 apply to this file:
 *       * Requiring preservation of specified reasonable legal notices or
 *         author attributions in that material or in the Appropriate Legal
 *         Notices displayed by works containing it.
 *       * Prohibiting misrepresentation of the origin of that material,
 *         or requiring that modified versions of such material be marked in
 *         reasonable ways as different from the original version.
 */

#include "mtp.hpp"
#include "util.hpp"

namespace MTP {
    void DataBuilder::addString(const std::string& utf8)
    {
        if (utf8.empty()) {
            // Not "1 followed by a null unit": the PTP empty string is a bare
            // zero length byte, and hosts that count units reject the longer form.
            addU8(0);
            return;
        }

        const std::u16string utf16 = StringUtils::UTF8toUTF16(utf8.c_str());
        // The length byte counts code units including the terminator, so it caps
        // out at 255 units. Truncate rather than overflow it; a filename that
        // long can't round-trip through MTP anyway.
        size_t units = utf16.size() + 1;
        if (units > 255) {
            units = 255;
        }

        addU8((uint8_t)units);
        for (size_t i = 0; i + 1 < units; i++) {
            addU16((uint16_t)utf16[i]);
        }
        addU16(0);
    }

    bool DataParser::require(size_t count)
    {
        if (!mOk || mPosition + count > mSize) {
            mOk = false;
            return false;
        }
        return true;
    }

    uint8_t DataParser::readU8(void)
    {
        if (!require(sizeof(uint8_t))) {
            return 0;
        }
        return mData[mPosition++];
    }

    uint16_t DataParser::readU16(void)
    {
        if (!require(sizeof(uint16_t))) {
            return 0;
        }
        uint16_t value;
        std::memcpy(&value, mData + mPosition, sizeof(value));
        mPosition += sizeof(value);
        return value;
    }

    uint32_t DataParser::readU32(void)
    {
        if (!require(sizeof(uint32_t))) {
            return 0;
        }
        uint32_t value;
        std::memcpy(&value, mData + mPosition, sizeof(value));
        mPosition += sizeof(value);
        return value;
    }

    uint64_t DataParser::readU64(void)
    {
        if (!require(sizeof(uint64_t))) {
            return 0;
        }
        uint64_t value;
        std::memcpy(&value, mData + mPosition, sizeof(value));
        mPosition += sizeof(value);
        return value;
    }

    std::string DataParser::readString(void)
    {
        const uint8_t units = readU8();
        if (units == 0) {
            return "";
        }

        std::u16string utf16;
        utf16.reserve(units);
        for (uint8_t i = 0; i < units; i++) {
            const uint16_t unit = readU16();
            if (!mOk) {
                return "";
            }
            // The terminator is included in the count; stop at it rather than
            // carrying an embedded null into the std::string.
            if (unit == 0) {
                break;
            }
            utf16.push_back((char16_t)unit);
        }
        return StringUtils::UTF16toUTF8(utf16);
    }

    void DataParser::skip(size_t count)
    {
        if (require(count)) {
            mPosition += count;
        }
    }
}
