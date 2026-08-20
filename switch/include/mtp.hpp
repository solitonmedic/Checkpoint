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

#ifndef MTP_HPP
#define MTP_HPP

#include <cstdint>
#include <cstring>
#include <string>
#include <switch.h>
#include <vector>

// Wire-level vocabulary of PTP (PIMA 15740 / USB Still Image class) and its MTP
// extension: the container header every bulk block starts with, the operation /
// response / property codes we answer, and the two little codecs that read and
// write PTP datasets.
//
// Everything here is protocol constants and byte shuffling; the actual
// behaviour lives in MTP::Responder (mtpresponder.hpp) and the USB plumbing in
// MTP::UsbInterface (mtpusb.hpp).
namespace MTP {
    // Every bulk block starts with this 12-byte little-endian header. `length`
    // counts the header itself, so a command with no parameters is 12 bytes and
    // one with five is 32.
    constexpr size_t CONTAINER_HEADER_SIZE = 12;
    // A command block carries at most five u32 parameters.
    constexpr size_t MAX_PARAMETERS = 5;

    enum ContainerType : uint16_t {
        ContainerType_Undefined = 0,
        ContainerType_Command   = 1,
        ContainerType_Data      = 2,
        ContainerType_Response  = 3,
        ContainerType_Event     = 4,
    };

    // Operation codes. The 0x1xxx block is base PTP, 0x9xxx is the MTP extension.
    enum OperationCode : uint16_t {
        Operation_GetDeviceInfo           = 0x1001,
        Operation_OpenSession             = 0x1002,
        Operation_CloseSession            = 0x1003,
        Operation_GetStorageIDs           = 0x1004,
        Operation_GetStorageInfo          = 0x1005,
        Operation_GetNumObjects           = 0x1006,
        Operation_GetObjectHandles        = 0x1007,
        Operation_GetObjectInfo           = 0x1008,
        Operation_GetObject               = 0x1009,
        Operation_DeleteObject            = 0x100B,
        Operation_SendObjectInfo          = 0x100C,
        Operation_SendObject              = 0x100D,
        Operation_GetDevicePropDesc       = 0x1014,
        Operation_GetDevicePropValue      = 0x1015,
        Operation_MoveObject              = 0x1019,
        Operation_GetPartialObject        = 0x101B,
        Operation_GetObjectPropsSupported = 0x9801,
        Operation_GetObjectPropDesc       = 0x9802,
        Operation_GetObjectPropValue      = 0x9803,
        Operation_SetObjectPropValue      = 0x9804,
        Operation_GetObjectPropList       = 0x9805,
        Operation_GetObjectReferences     = 0x9810,
    };

    enum ResponseCode : uint16_t {
        Response_Ok                              = 0x2001,
        Response_GeneralError                    = 0x2002,
        Response_SessionNotOpen                  = 0x2003,
        Response_InvalidTransactionId            = 0x2004,
        Response_OperationNotSupported           = 0x2005,
        Response_ParameterNotSupported           = 0x2006,
        Response_IncompleteTransfer              = 0x2007,
        Response_InvalidStorageId                = 0x2008,
        Response_InvalidObjectHandle             = 0x2009,
        Response_DevicePropNotSupported          = 0x200A,
        Response_InvalidObjectFormatCode         = 0x200B,
        Response_StoreFull                       = 0x200C,
        Response_ObjectWriteProtected            = 0x200D,
        Response_StoreReadOnly                   = 0x200E,
        Response_AccessDenied                    = 0x200F,
        Response_PartialDeletion                 = 0x2012,
        Response_StoreNotAvailable               = 0x2013,
        Response_NoValidObjectInfo               = 0x2015,
        Response_DeviceBusy                      = 0x2019,
        Response_InvalidParentObject             = 0x201A,
        Response_InvalidParameter                = 0x201D,
        Response_SessionAlreadyOpen              = 0x201E,
        Response_TransactionCancelled            = 0x201F,
        Response_InvalidObjectPropCode           = 0xA801,
        Response_InvalidObjectPropValue          = 0xA803,
        Response_InvalidDataset                  = 0xA806,
        Response_SpecificationByGroupUnsupported = 0xA807,
        Response_SpecificationByDepthUnsupported = 0xA808,
        Response_ObjectPropNotSupported          = 0xA80A,
    };

    // Object formats. We only ever report a plain file or a folder: Checkpoint's
    // storage is a save-backup tree, not a media library, so claiming a richer
    // format would only invite hosts to ask for metadata that doesn't exist.
    enum ObjectFormat : uint16_t {
        ObjectFormat_Undefined   = 0x3000,
        ObjectFormat_Association = 0x3001,
    };

    enum AssociationType : uint16_t {
        AssociationType_Undefined     = 0x0000,
        AssociationType_GenericFolder = 0x0001,
    };

    enum DataType : uint16_t {
        DataType_Int8    = 0x0001,
        DataType_UInt8   = 0x0002,
        DataType_Int16   = 0x0003,
        DataType_UInt16  = 0x0004,
        DataType_Int32   = 0x0005,
        DataType_UInt32  = 0x0006,
        DataType_Int64   = 0x0007,
        DataType_UInt64  = 0x0008,
        DataType_UInt128 = 0x000A,
        DataType_AUInt16 = 0x4004,
        DataType_AUInt32 = 0x4006,
        DataType_String  = 0xFFFF,
    };

    enum DevicePropertyCode : uint16_t {
        DeviceProperty_BatteryLevel           = 0x5001,
        DeviceProperty_SynchronizationPartner = 0xD401,
        DeviceProperty_DeviceFriendlyName     = 0xD402,
    };

    enum ObjectPropertyCode : uint16_t {
        ObjectProperty_StorageId                        = 0xDC01,
        ObjectProperty_ObjectFormat                     = 0xDC02,
        ObjectProperty_ProtectionStatus                 = 0xDC03,
        ObjectProperty_ObjectSize                       = 0xDC04,
        ObjectProperty_ObjectFileName                   = 0xDC07,
        ObjectProperty_DateCreated                      = 0xDC08,
        ObjectProperty_DateModified                     = 0xDC09,
        ObjectProperty_ParentObject                     = 0xDC0B,
        ObjectProperty_PersistentUniqueObjectIdentifier = 0xDC41,
        ObjectProperty_Name                             = 0xDC44,
    };

    // The one storage we expose. Per PTP the high 16 bits are the physical store
    // and the low 16 the logical partition; a single non-zero pair is all a
    // one-volume device needs.
    constexpr uint32_t STORAGE_ID = 0x00010001;

    // Parent handle sentinels. 0xFFFFFFFF is "the storage root" in every PTP
    // request that takes a parent; 0 means "no filter"/"all objects" on
    // GetObjectHandles, which for a rooted tree is the root as well.
    constexpr uint32_t HANDLE_ROOT = 0xFFFFFFFF;
    constexpr uint32_t HANDLE_ALL  = 0x00000000;

    // A parsed container header plus the parameters that followed it.
    struct Container {
        uint32_t length;
        uint16_t type;
        uint16_t code;
        uint32_t transactionId;
        uint32_t parameters[MAX_PARAMETERS];
        // How many parameters the block actually carried; reading past this
        // yields 0, which is what PTP says an omitted parameter means.
        size_t parameterCount;

        uint32_t parameter(size_t index) const { return index < parameterCount ? parameters[index] : 0; }
    };

    // Appends PTP datasets to a growable buffer. All scalars go out
    // little-endian, which on aarch64 is a straight memcpy.
    class DataBuilder {
    public:
        explicit DataBuilder(size_t reserve = 256) { mBuffer.reserve(reserve); }

        void addU8(uint8_t value) { mBuffer.push_back(value); }
        void addU16(uint16_t value) { append(&value, sizeof(value)); }
        void addU32(uint32_t value) { append(&value, sizeof(value)); }
        void addU64(uint64_t value) { append(&value, sizeof(value)); }

        // A 128-bit value, low half first. Used for the persistent object id.
        void addU128(uint64_t low, uint64_t high)
        {
            addU64(low);
            addU64(high);
        }

        // PTP string: a byte count of UTF-16 code units *including* the
        // terminator, then the UTF-16LE units. An empty string is a single 0
        // byte with no terminator at all - hosts are strict about that.
        void addString(const std::string& utf8);

        void addArrayU16(const std::vector<uint16_t>& values)
        {
            addU32((uint32_t)values.size());
            for (uint16_t value : values) {
                addU16(value);
            }
        }

        void addArrayU32(const std::vector<uint32_t>& values)
        {
            addU32((uint32_t)values.size());
            for (uint32_t value : values) {
                addU32(value);
            }
        }

        void addRaw(const void* data, size_t size) { append(data, size); }

        const std::vector<uint8_t>& data(void) const { return mBuffer; }
        size_t size(void) const { return mBuffer.size(); }

    private:
        void append(const void* data, size_t size)
        {
            const uint8_t* bytes = static_cast<const uint8_t*>(data);
            mBuffer.insert(mBuffer.end(), bytes, bytes + size);
        }

        std::vector<uint8_t> mBuffer;
    };

    // Reads PTP datasets out of a received buffer. Every read past the end sets
    // the failure flag and returns a zero value, so a truncated or malformed
    // dataset from the host degrades into "ok() == false" instead of reading
    // out of bounds.
    class DataParser {
    public:
        DataParser(const void* data, size_t size) : mData(static_cast<const uint8_t*>(data)), mSize(size) {}

        uint8_t readU8(void);
        uint16_t readU16(void);
        uint32_t readU32(void);
        uint64_t readU64(void);
        std::string readString(void);
        // Skips `count` bytes; sets the failure flag if that runs off the end.
        void skip(size_t count);

        bool ok(void) const { return mOk; }
        size_t remaining(void) const { return mOk && mSize > mPosition ? mSize - mPosition : 0; }

    private:
        bool require(size_t count);

        const uint8_t* mData;
        size_t mSize;
        size_t mPosition = 0;
        bool mOk         = true;
    };
}

#endif
