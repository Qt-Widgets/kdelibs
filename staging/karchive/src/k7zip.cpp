/* This file is part of the KDE libraries
   Copyright (C) 2011 Mario Bensi <mbensi@ipsquad.net>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License version 2 as published by the Free Software Foundation.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public License
   along with this library; see the file COPYING.LIB.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.
*/

#include "k7zip.h"

#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QBuffer>
#include <QtCore/QFile>

#include "kcompressiondevice.h"
#include <kfilterbase.h>


#include <time.h> // time()
#include "zlib.h"


////////////////////////////////////////////////////////////////////////
/////////////////////////// K7Zip //////////////////////////////////////
////////////////////////////////////////////////////////////////////////

unsigned char k7zip_signature[6] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};
unsigned char XZ_HEADER_MAGIC[6] = { 0xFD, '7', 'z', 'X', 'Z', 0x00 };

#define GetUi16(p, offset) (((unsigned char)p[offset+0]) | (((unsigned char)p[1]) << 8))

#define GetUi32(p, offset) ( \
                     ((unsigned char)p[offset+0])        | \
                     (((unsigned char)p[offset+1]) <<  8) | \
                     (((unsigned char)p[offset+2]) << 16) | \
                     (((unsigned char)p[offset+3]) << 24))

#define GetUi64(p, offset) ((quint32)GetUi32(p, offset) | (((quint64)GetUi32(p, offset + 4)) << 32))

#define LZMA2_DIC_SIZE_FROM_PROP(p) (((quint32)2 | ((p) & 1)) << ((p) / 2 + 11))

#define FILE_ATTRIBUTE_READONLY             1
#define FILE_ATTRIBUTE_HIDDEN               2
#define FILE_ATTRIBUTE_SYSTEM               4
#define FILE_ATTRIBUTE_DIRECTORY           16
#define FILE_ATTRIBUTE_ARCHIVE             32
#define FILE_ATTRIBUTE_DEVICE              64
#define FILE_ATTRIBUTE_NORMAL             128
#define FILE_ATTRIBUTE_TEMPORARY          256
#define FILE_ATTRIBUTE_SPARSE_FILE        512
#define FILE_ATTRIBUTE_REPARSE_POINT     1024
#define FILE_ATTRIBUTE_COMPRESSED        2048
#define FILE_ATTRIBUTE_OFFLINE          0x1000
#define FILE_ATTRIBUTE_ENCRYPTED        0x4000
#define FILE_ATTRIBUTE_UNIX_EXTENSION   0x8000   /* trick for Unix */

enum HeaderType
{
    kEnd,

    kHeader,

    kArchiveProperties,

    kAdditionalStreamsInfo,
    kMainStreamsInfo,
    kFilesInfo,

    kPackInfo,
    kUnpackInfo,
    kSubStreamsInfo,

    kSize,
    kCRC,

    kFolder,

    kCodersUnpackSize,
    kNumUnpackStream,

    kEmptyStream,
    kEmptyFile,
    kAnti,

    kName,
    kCTime,
    kATime,
    kMTime,
    kAttributes,
    kComment,

    kEncodedHeader,

    kStartPos,
    kDummy
};

// Method ID
static const quint64 k_Copy = 0x00;
static const quint64 k_Delta = 0x03;
static const quint64 k_x86 = 0x04; //BCJ
static const quint64 k_PPC = 0x05; // BIG Endian
static const quint64 k_IA64 = 0x06;
static const quint64 k_ARM = 0x07; // little Endian
static const quint64 k_ARM_Thumb = 0x08; // little Endian
static const quint64 k_SPARC = 0x09;
static const quint64 k_LZMA2 = 0x21;
static const quint64 k_Swap2 = 0x020302;
static const quint64 k_Swap4 = 0x020304;
static const quint64 k_LZMA = 0x030101;
static const quint64 k_BCJ = 0x03030103;
static const quint64 k_BCJ2 = 0x0303011B;
static const quint64 k_7zPPC = 0x03030205;
static const quint64 k_Alpha = 0x03030301;
static const quint64 k_7zIA64 = 0x03030401;
static const quint64 k_7zARM = 0x03030501;
static const quint64 k_M68 = 0x03030605; //Big Endian
static const quint64 k_ARMT = 0x03030701;
static const quint64 k_7zSPARC = 0x03030805;
static const quint64 k_PPMD = 0x030401;
static const quint64 k_Experimental = 0x037F01;
static const quint64 k_Shrink = 0x040101;
static const quint64 k_Implode = 0x040106;
static const quint64 k_Deflate = 0x040108;
static const quint64 k_Deflate64 = 0x040109;
static const quint64 k_Imploding = 0x040110;
static const quint64 k_Jpeg = 0x040160;
static const quint64 k_WavPack = 0x040161;
static const quint64 k_PPMd = 0x040162;
static const quint64 k_wzAES = 0x040163;
static const quint64 k_BZip2 = 0x040202;
static const quint64 k_Rar15 = 0x040301;
static const quint64 k_Rar20 = 0x040302;
static const quint64 k_Rar29 = 0x040303;
static const quint64 k_Arj = 0x040401; //1 2 3
static const quint64 k_Arj4 = 0x040402;
static const quint64 k_Z = 0x0405;
static const quint64 k_Lzh = 0x0406;
static const quint64 k_Cab = 0x0408;
static const quint64 k_DeflateNSIS = 0x040901;
static const quint64 k_Bzip2NSIS = 0x040902;
static const quint64 k_AES = 0x06F10701;


class FileInfo
{
public:
    FileInfo()
      : attribDefined(false)
      , attributes(0)
      , hasStream(false)
      , isDir(false)
      , size(0)
      , crc(0)
      , crcDefined(false)
    {}

    QString path;
    bool attribDefined;
    quint32 attributes;
    bool hasStream;
    bool isDir;
    quint64 size;
    quint32 crc;
    bool crcDefined;
};

class Folder
{
public:
    class FolderInfo
    {
    public:
        FolderInfo()
            : numInStreams(0)
            , numOutStreams(0)
            , methodID(0)
        {
        }

        bool isSimpleCoder() const { return (numInStreams == 1) && (numOutStreams == 1); }

        int numInStreams;
        int numOutStreams;
        QVector<unsigned char> properties;
        quint64 methodID;
    };

    Folder()
        : unpackCRCDefined(false)
        , unpackCRC(0)
    {}

    bool unpackCRCDefined;
    quint32 unpackCRC;
    QVector<FolderInfo*> folderInfos;
    QVector<quint64> inIndexes;
    QVector<quint64> outIndexes;
    QVector<quint64> packedStreams;
    QVector<quint64> unpackSizes;
};

class K7Zip::K7ZipPrivate
{
public:
    K7ZipPrivate(K7Zip *parent)
      : q(parent),
        packPos(0),
        numPackStreams(0),
        buffer(0),
        pos(0),
        end(0),
        headerSize(0),
        countSize(0)
    {
    }

    K7Zip *q;

    QVector<bool> packCRCsDefined;
    QVector<quint32> packCRCs;
    QVector<quint64> numUnpackStreamsInFolders;

    QVector<Folder*> folders;
    QVector<FileInfo*> fileInfos;
    // File informations
    QVector<bool> cTimesDefined;
    QVector<quint64> cTimes;
    QVector<bool> aTimesDefined;
    QVector<quint64> aTimes;
    QVector<bool> mTimesDefined;
    QVector<quint64> mTimes;
    QVector<bool> startPositionsDefined;
    QVector<quint64> startPositions;
    QVector<int> fileInfoPopIDs;

    quint64 packPos;
    quint64 numPackStreams;
    QVector<quint64> packSizes;
    QVector<quint64> unpackSizes;
    QVector<bool> digestsDefined;
    QVector<quint32> digests;

    QVector<bool> isAnti;

    const char* buffer;
    quint64 pos;
    quint64 end;
    quint64 headerSize;
    quint64 countSize;

    //Write
    QByteArray header;
    QByteArray outData; // Store data in this buffer before compress and write in archive.

    // Read
    int readByte();
    quint32 readUInt32();
    quint64 readUInt64();
    quint64 readNumber();
    QString readString();
    void readHashDigests(int numItems, QVector<bool> &digestsDefined, QVector<quint32> &digests);
    void readBoolVector(int numItems, QVector<bool> &v);
    void readBoolVector2(int numItems, QVector<bool> &v);
    void skipData(int size);
    bool findAttribute(int attribute);
    bool readUInt64DefVector(int numFiles, QVector<quint64> &values, QVector<bool> &defined);

    Folder* folderItem();
    bool readMainStreamsInfo();
    bool readPackInfo();
    bool readUnpackInfo();
    bool readSubStreamsInfo();
    QByteArray readAndDecodePackedStreams(bool readMainStreamInfo = true);

    //Write
    void writeByte(unsigned char b);
    void writeNumber(quint64 value);
    void writeBoolVector(const QVector<bool> &boolVector);
    void writeUInt32(quint32 value);
    void writeUInt64(quint64 value);
    void writeHashDigests(const QVector<bool> &digestsDefined, const QVector<quint32> &digests);
    void writeAlignedBoolHeader(const QVector<bool> &v, int numDefined, int type, unsigned itemSize);
    void writeUInt64DefVector(const QVector<quint64> &v, const QVector<bool> defined, int type);
    void writeFolder(const Folder *folder);
    void writePackInfo(quint64 dataOffset, QVector<quint64> &packedSizes, QVector<bool> &packedCRCsDefined, QVector<quint32> &packedCRCs);
    void writeUnpackInfo(QVector<Folder*> &folderItems);
    void writeSubStreamsInfo(const QVector<quint64> &unpackSizes, const QVector<bool> &digestsDefined, const QVector<quint32> &digests);
    void writeHeader(quint64 &headerOffset);
    void writeSignature();
    void writeStartHeader(const quint64 nextHeaderSize, const quint32 nextHeaderCRC, const quint64 nextHeaderOffset);
    QByteArray encodeStream(QVector<quint64> &packSizes, QVector<Folder*> &folds);
};

K7Zip::K7Zip( const QString& fileName )
    : KArchive( fileName ), d(new K7ZipPrivate(this))
{
}

K7Zip::K7Zip( QIODevice * dev )
    : KArchive( dev ), d(new K7ZipPrivate(this))
{
    Q_ASSERT( dev );
}

K7Zip::~K7Zip()
{
    if( isOpen() )
        close();

    delete d;
}

int K7Zip::K7ZipPrivate::readByte()
{
    if (!buffer || pos+1 > end) {
        return -1;
    }
    return buffer[pos++];
}

quint32 K7Zip::K7ZipPrivate::readUInt32()
{
    if (!buffer || (quint64)(pos + 4) > end) {
        qDebug() << "error size";
        return 0;
    }

    quint32 res = GetUi32(buffer, pos);
    pos += 4;
    return res;
}

quint64 K7Zip::K7ZipPrivate::readUInt64()
{
    if (!buffer || (quint64)(pos + 8) > end) {
        qDebug() << "error size";
        return 0;
    }

    quint64 res = GetUi64(buffer, pos);
    pos += 8;
    return res;
}

quint64 K7Zip::K7ZipPrivate::readNumber()
{
    if (!buffer) {
        return 0;
    }

    unsigned char firstByte = buffer[pos++];
    unsigned char mask = 0x80;
    quint64 value = 0;
    for (int i = 0; i < 8; i++) {
        if ((firstByte & mask) == 0) {
            quint64 highPart = firstByte & (mask - 1);
            value += (highPart << (i * 8));
            return value;
        }
        value |= ((unsigned char)buffer[pos++] << (8 * i));
        mask >>= 1;
    }
    return value;
}

QString K7Zip::K7ZipPrivate::readString()
{
    if (!buffer) {
        return QString();
    }

    const char *buf = buffer + pos;
    size_t rem = (end - pos) / 2 * 2;
    {
        size_t i;
        for (i = 0; i < rem; i += 2) {
            if (buf[i] == 0 && buf[i + 1] == 0) {
                break;
            }
        }
        if (i == rem) {
            qDebug() << "read string error";
            return QString();
        }
        rem = i;
    }

    int len = (int)(rem / 2);
    if (len < 0 || (size_t)len * 2 != rem) {
        qDebug() << "read string unsupported";
        return QString();
    }

    QString p;
    for (int i = 0; i < len; i++, buf += 2) {
        p += (wchar_t)GetUi16(buf, 0);
    }

    pos += rem + 2;
    return p;
}

void K7Zip::K7ZipPrivate::skipData(int size)
{
    if (!buffer || pos + size > end) {
        return;
    }
    pos += size;
}

bool K7Zip::K7ZipPrivate::findAttribute(int attribute)
{
    if (!buffer) {
        return false;
    }

    for (;;)
    {
        int type = readByte();
        if (type == attribute) {
            return true;
        }
        if (type == kEnd) {
        return false;
        }
        skipData(readNumber());
    }
}


void K7Zip::K7ZipPrivate::readBoolVector(int numItems, QVector<bool> &v)
{
    if (!buffer) {
        return;
    }

    unsigned char b = 0;
    unsigned char mask = 0;
    for (int i = 0; i < numItems; i++) {
        if (mask == 0) {
            b = readByte();
            mask = 0x80;
        }
        v.append((b & mask) != 0);
        mask >>= 1;
    }
}

void K7Zip::K7ZipPrivate::readBoolVector2(int numItems, QVector<bool> &v)
{
    if (!buffer) {
        return;
    }

    int allAreDefined = readByte();
    if (allAreDefined == 0) {
        readBoolVector(numItems, v);
        return;
    }

    for (int i = 0; i < numItems; i++) {
        v.append(true);
    }
}

void K7Zip::K7ZipPrivate::readHashDigests(int numItems,
                                          QVector<bool> &digestsDefined,
                                          QVector<quint32> &digests)
{
    if (!buffer) {
        return;
    }

    readBoolVector2(numItems, digestsDefined);
    for (int i = 0; i < numItems; i++)
    {
        quint32 crc = 0;
        if (digestsDefined[i]) {
            crc = GetUi32(buffer, pos);
            pos += 4;
        }
        digests.append(crc);
    }
}

Folder* K7Zip::K7ZipPrivate::folderItem()
{
    if (!buffer) {
        return false;
    }

    Folder* folder = new Folder;
    int numCoders = readNumber();

    quint64 numInStreamsTotal = 0;
    quint64 numOutStreamsTotal = 0;
    for (int i = 0; i < numCoders; i++) {
        Folder::FolderInfo* info = new Folder::FolderInfo();
        //BYTE 
        //    {
        //      0:3 CodecIdSize
        //      4:  Is Complex Coder
        //      5:  There Are Attributes
        //      6:  Reserved
        //      7:  There are more alternative methods. (Not used
        //      anymore, must be 0).
        //    }
        unsigned char coderInfo = readByte();
        int codecIdSize = (coderInfo & 0xF);
        if (codecIdSize > 8) {
            qDebug() << "unsupported codec id size";
            delete folder;
            return 0;
        }
        unsigned char codecID[codecIdSize];
        for (int i=0; i < codecIdSize; ++i) {
            codecID[i] = readByte();
        }

        int id = 0;
        for (int j = 0; j < codecIdSize; j++) {
            id |= codecID[codecIdSize - 1 - j] << (8 * j);
        }
        info->methodID = id;

        //if (Is Complex Coder)
        if ((coderInfo & 0x10) != 0) {
            info->numInStreams = readNumber();
            info->numOutStreams = readNumber();
        } else {
            info->numInStreams = 1;
            info->numOutStreams = 1;
        }

        //if (There Are Attributes)
        if ((coderInfo & 0x20) != 0) {
            int propertiesSize = readNumber();
            for (int i=0; i < propertiesSize; ++i) {
                info->properties.append(readByte());
            }
        }

        if ((coderInfo & 0x80) != 0) {
            qDebug() << "unsupported";
            delete folder;
            return 0;
        }

        numInStreamsTotal += info->numInStreams;
        numOutStreamsTotal += info->numOutStreams;
        folder->folderInfos.append(info);
    }

    int numBindPairs = numOutStreamsTotal - 1;
    for (int i = 0; i < numBindPairs; i++) {
        folder->inIndexes.append(readNumber());
        folder->outIndexes.append(readNumber());
    }

    int numPackedStreams = numInStreamsTotal - numBindPairs;
    if (numPackedStreams > 1) {
        for (int i = 0; i < numPackedStreams; ++i) {
            folder->packedStreams.append(readNumber());
        }
    } else {
        if (numPackedStreams == 1) {
            for (quint64 i = 0; i < numInStreamsTotal; i++) {
                bool found = false;
                for (int j = 0; j < folder->inIndexes.size(); ++j) {
                    if (folder->inIndexes[j] == i) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    folder->packedStreams.append(i);
                    break;
                }
            }
            if (folder->packedStreams.size() != 1) {
                delete folder;
                return 0;
            }
        }
    }
    return folder;
}

bool K7Zip::K7ZipPrivate::readUInt64DefVector(int numFiles, QVector<quint64>& values, QVector<bool>& defined)
{
    if (!buffer) {
        return false;
    }

    readBoolVector2(numFiles, defined);

    int external = readByte();
    if (external != 0) {
        int dataIndex = readNumber();
        if (dataIndex < 0 /*|| dataIndex >= dataVector->Size()*/) {
            qDebug() << "wrong data index";
            return false;
        }

        // TODO : go to the new index
    }

    for (int i = 0; i < numFiles; i++)
    {
        quint64 t = 0;
        if (defined[i]) {
            t = readUInt64();
        }
        values.append(t);
    }
    return true;
}

bool K7Zip::K7ZipPrivate::readPackInfo()
{
    if (!buffer) {
        return false;
    }

    packPos = readNumber();
    numPackStreams = readNumber();
    packSizes.clear();

    packCRCsDefined.clear();
    packCRCs.clear();

    if (!findAttribute(kSize)) {
        qDebug() << "kSize not found";
        return false;
    }

    for (quint64 i = 0; i < numPackStreams; ++i) {
        packSizes.append(readNumber());
    }

    int type;
    for (;;) {
        type = readByte();
        if (type == kEnd) {
            break;
        }
        if (type == kCRC) {
            readHashDigests(numPackStreams, packCRCsDefined, packCRCs);
            continue;
        }
        skipData(readNumber());
    }

    if (packCRCs.isEmpty()) {
        for (quint64 i = 0; i < numPackStreams; ++i) {
            packCRCsDefined.append(false);
            packCRCs.append(0);
        }
    }
    return true;
}

bool K7Zip::K7ZipPrivate::readUnpackInfo()
{
    if (!buffer) {
        return false;
    }

    if (!findAttribute(kFolder)) {
        qDebug() << "kFolder not found";
        return false;
    }

    int numFolders = readNumber();
    folders.clear();
    int external = readByte();
    switch (external) {
    case 0:
    {
        for (int i = 0; i < numFolders; ++i) {
            folders.append(folderItem());
        }
        break;
    }
    case 1:
    {
        int dataIndex = readNumber();
        if (dataIndex < 0 /*|| dataIndex >= dataVector->Size()*/) {
            qDebug() << "wrong data index";
        }
        // TODO : go to the new index
        break;
    }
    default:
        qDebug() << "external error";
        return false;
    }


    if(!findAttribute(kCodersUnpackSize)) {
        qDebug() << "kCodersUnpackSize not found";
        return false;
    }

    for (int i = 0; i < numFolders; ++i) {
        Folder* folder = folders[i];
        int numOutStreams = 0;
        for (int j = 0; j < folder->folderInfos.size(); ++j) {
            numOutStreams += folder->folderInfos[i]->numOutStreams;
        }
        for (int j = 0; j < numOutStreams; ++j) {
            folder->unpackSizes.append(readNumber());
        }
    }

    for (;;) {
        int type = readByte();
        if (type == kEnd) {
            break;
        }
        if (type == kCRC) {
            QVector<bool> crcsDefined;
            QVector<quint32> crcs;
            readHashDigests(numFolders, crcsDefined, crcs);
            for (int i = 0; i < numFolders; i++)
            {
                Folder* folder = folders[i];
                folder->unpackCRCDefined = crcsDefined[i];
                folder->unpackCRC = crcs[i];
            }
            continue;
        }
        skipData(readNumber());
    }
    return true;
}

bool K7Zip::K7ZipPrivate::readSubStreamsInfo()
{
    if (!buffer) {
        return false;
    }

    numUnpackStreamsInFolders.clear();

    int type;
    for (;;) {
        type = readByte();
        if (type == kNumUnpackStream) {
            for (int i = 0; i < folders.size(); i++) {
                numUnpackStreamsInFolders.append(readNumber());
            }
            continue;
        }
        if (type == kCRC || type == kSize)
            break;
        if (type == kEnd)
            break;
        skipData(readNumber());
    }

    if (numUnpackStreamsInFolders.isEmpty()) {
        for (int i = 0; i < folders.size(); i++) {
            numUnpackStreamsInFolders.append(1);
        }
    }

    for (int i = 0; i < numUnpackStreamsInFolders.size(); i++)
    {
        quint64 numSubstreams = numUnpackStreamsInFolders[i];
        if (numSubstreams == 0)
            continue;
        quint64 sum = 0;
        for (quint64 j = 1; j < numSubstreams; j++) {
            if (type == kSize)
            {
                int size = readNumber();
                unpackSizes.append(size);
                sum += size;
            }
        }
        if (!folders[i]->unpackSizes.isEmpty()) {
            for (int j = folders[i]->unpackSizes.size() - 1; j >= 0; j--) {
                bool found = false;
                for (int k = 0; j < folders[i]->inIndexes.size(); ++k) {
                    if (folders[i]->inIndexes[k] == (quint64)i) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    unpackSizes.append(folders[i]->unpackSizes[j] - sum);
                }
            }
        }
    }

    if (type == kSize)
        type = readByte();

    int numDigests = 0;
    int numDigestsTotal = 0;
    for (int i = 0; i < folders.size(); i++)
    {
        quint64 numSubstreams = numUnpackStreamsInFolders[i];
        if (numSubstreams != 1 || !folders[i]->unpackCRCDefined) {
            numDigests += numSubstreams;
        }
        numDigestsTotal += numSubstreams;
    }

    for (;;) {
        if (type == kCRC) {
            QVector<bool> digestsDefined2;
            QVector<quint32> digests2;
            readHashDigests(numDigests, digestsDefined2, digests2);
            int digestIndex = 0;
            for (int i = 0; i < folders.size(); i++)
            {
                quint64 numSubstreams = numUnpackStreamsInFolders[i];
                const Folder* folder = folders[i];
                if (numSubstreams == 1 && folder->unpackCRCDefined) {
                    digestsDefined.append(true);
                    digests.append(folder->unpackCRC);
                } else {
                    for (quint64 j = 0; j < numSubstreams; j++, digestIndex++) {
                        digestsDefined.append(digestsDefined2[digestIndex]);
                        digests.append(digests2[digestIndex]);
                    }
                }
            }
        } else if (type == kEnd) {
            if (digestsDefined.isEmpty()) {
                for (int i = 0; i < numDigestsTotal; i++) {
                    digestsDefined.append(false);
                    digests.append(0);
                }
            }

            break;
        } else {
            skipData(readNumber());
        }

        type = readByte();
    }
    return true;
}

#define TICKSPERSEC        10000000
#define TICKSPERMSEC       10000
#define SECSPERDAY         86400
#define SECSPERHOUR        3600
#define SECSPERMIN         60
#define EPOCHWEEKDAY       1  /* Jan 1, 1601 was Monday */
#define DAYSPERWEEK        7
#define DAYSPERQUADRICENTENNIUM (365 * 400 + 97)
#define DAYSPERNORMALQUADRENNIUM (365 * 4 + 1)
#define TICKS_1601_TO_1970 (SECS_1601_TO_1970 * TICKSPERSEC)
#define SECS_1601_TO_1970  ((369 * 365 + 89) * (unsigned long long)SECSPERDAY)

static time_t toTimeT(const long long liTime)
{
    long long time = liTime / TICKSPERSEC;

    /* The native version of RtlTimeToTimeFields does not take leap seconds
     * into account */

    /* Split the time into days and seconds within the day */
    long int days = time / SECSPERDAY;
    int secondsInDay = time % SECSPERDAY;

    /* compute time of day */
    short hour = (short) (secondsInDay / SECSPERHOUR);
    secondsInDay = secondsInDay % SECSPERHOUR;
    short minute = (short) (secondsInDay / SECSPERMIN);
    short second = (short) (secondsInDay % SECSPERMIN);

    /* compute year, month and day of month. */
    long int cleaps=( 3 * ((4 * days + 1227) / DAYSPERQUADRICENTENNIUM) + 3 ) / 4;
    days += 28188 + cleaps;
    long int years = (20 * days - 2442) / (5 * DAYSPERNORMALQUADRENNIUM);
    long int yearday = days - (years * DAYSPERNORMALQUADRENNIUM)/4;
    long int months = (64 * yearday) / 1959;
    /* the result is based on a year starting on March.
     * To convert take 12 from Januari and Februari and
     * increase the year by one. */

    short month, year;
    if( months < 14 ) {
        month = (short)(months - 1);
        year = (short)(years + 1524);
    } else {
        month = (short)(months - 13);
        year = (short)(years + 1525);
    }
    /* calculation of day of month is based on the wonderful
     * sequence of INT( n * 30.6): it reproduces the·
     * 31-30-31-30-31-31 month lengths exactly for small n's */
    short day = (short)(yearday - (1959 * months) / 64 );

    QDateTime t(QDate(year, month, day), QTime(hour, minute, second));
    t.setTimeSpec(Qt::UTC);
    return  t.toTime_t();
}

long long rtlSecondsSince1970ToSpecTime(quint32 seconds) {
    long long secs = seconds * (long long)TICKSPERSEC + TICKS_1601_TO_1970;
    return secs;
}


bool K7Zip::K7ZipPrivate::readMainStreamsInfo()
{
    if (!buffer) {
        return false;
    }

    quint32 type;
    for (;;) {
        type = readByte();
        if (type > ((quint32)1 << 30)) {
            qDebug() << "type error";
            return false;
        }
        switch(type)
        {
        case kEnd:
            return true;
        case kPackInfo:
        {
            if (!readPackInfo()) {
                qDebug() << "error during read pack information";
                return false;
            }
            break;
        }
        case kUnpackInfo:
        {
            if (!readUnpackInfo()) {
                qDebug() << "error during read pack information";
                return false;
            }
            break;
        }
        case kSubStreamsInfo:
        {
            if (!readSubStreamsInfo()) {
                qDebug() << "error during read substreams information";
                return false;
            }
            break;
        }
        default:
            qDebug() << "Wrong type";
            return false;
        }
    }

    qDebug() << "should not reach";
    return false;
}

QByteArray K7Zip::K7ZipPrivate::readAndDecodePackedStreams(bool readMainStreamInfo)
{
    if (!buffer) {
        return QByteArray();
    }

    if (readMainStreamInfo)
        readMainStreamsInfo();

    QByteArray inflatedData;

    int packIndex = 0;
    for (int i = 0; i < folders.size(); i++)
    {
        const Folder* folder = folders[i];
        quint64 unpackSize64 = 0;
        // GetUnpackSize
        if (!folder->unpackSizes.isEmpty()) {
            for (int j = folder->unpackSizes.size() - 1; j >= 0; j--) {
                bool found = false;
                for (int k = 0; k < folder->outIndexes.size(); ++k) {
                    if (folder->outIndexes[j] == folder->unpackSizes[j]) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    unpackSize64 = folder->unpackSizes[j];
                    break;
                }
            }
        }
        size_t unpackSize = (size_t)unpackSize64;
        if (unpackSize != unpackSize64) {
            qDebug() << "unsupported";
            return inflatedData;
        }

        char encodedBuffer[packSizes[packIndex]];

        quint64 method;
        quint32 dicSize = 0;
        for (int g = 0; g < folder->folderInfos.size(); ++g) {
            Folder::FolderInfo* info = folder->folderInfos[g];
            switch (info->methodID) {
            case k_LZMA:
                method = k_LZMA;
                if (info->properties.size() == 5) {
                    dicSize = ((unsigned char)info->properties[1]        |
                              (((unsigned char)info->properties[2]) <<  8) |
                              (((unsigned char)info->properties[3]) << 16) |
                              (((unsigned char)info->properties[4]) << 24));
                }
                break;
            case k_LZMA2:
                method = k_LZMA2;
                if (info->properties.size() == 1) {
                    quint32 p = info->properties[0];
                    dicSize = (((quint32)2 | ((p) & 1)) << ((p) / 2 + 11));
                }
                break;
            case k_PPMD:
                method = k_PPMD;
                if (info->properties.size() == 5) {
                    //Byte order = *(const Byte *)coder.Props;
                    dicSize = ((unsigned char)info->properties[1]        |
                              (((unsigned char)info->properties[2]) <<  8) |
                              (((unsigned char)info->properties[3]) << 16) |
                              (((unsigned char)info->properties[4]) << 24));
                    break;
                }
            case k_AES:
                if (info->properties.size() >=1) {
                    //const Byte *data = (const Byte *)coder.Props;
                    //Byte firstByte = *data++;
                    //UInt32 numCyclesPower = firstByte & 0x3F;
                }

                break;
            }
        }

        qint64 packPosition = packIndex == 0 ? packPos + 32 /*header size*/ : packPos + packSizes[packIndex-1];

        QIODevice* dev = q->device();
        dev->seek(packPosition);
        quint64 n = dev->read(encodedBuffer, packSizes[packIndex]);
        if ( n != packSizes[packIndex] ) {
            qDebug() << "Failed read next header size, should read " << packSizes[packIndex] << ", read " << n;
            return inflatedData;
        }

        // Create Filter
        KFilterBase* filter = KCompressionDevice::filterForCompressionType(KCompressionDevice::Xz);
        if (!filter) {
            qDebug() << "filter not found";
            return inflatedData;
        }


        filter->init(QIODevice::ReadOnly);
        QByteArray deflatedData(encodedBuffer, packSizes[packIndex]);

        if (method == k_LZMA) {
            const unsigned char lzmaHeader[13] = {0x5d, 0x00, 0x00, 0x80, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

            for (int i = 12; i >= 0; --i) {
                deflatedData.prepend(lzmaHeader[i]);
            }
        } else {
            const unsigned char lzma2Header[18] = {0x00, 0x04, 0xe6, 0xd6, 0xb4, 0x46, 0x02, 0x00, 0x21, 0x01, 0x16, 0x00, 0x00, 0x00, 0x74, 0x2f, 0xe5, 0xa3};
            for (int i = 17; i >= 0; --i) {
                deflatedData.prepend(lzma2Header[i]);
            }
            for (int i = 5; i >= 0; --i) {
                deflatedData.prepend(XZ_HEADER_MAGIC[i]);
            }
        }

        filter->setInBuffer(deflatedData.data(), deflatedData.size());

        QByteArray outBuffer;
        // reserve memory
        outBuffer.resize(unpackSize);

        KFilterBase::Result result = KFilterBase::Ok;
        while (result != KFilterBase::End && result != KFilterBase::Error && !filter->inBufferEmpty()) {
            filter->setOutBuffer(outBuffer.data(), outBuffer.size());
            result = filter->uncompress();
            if (result == KFilterBase::Error) {
                qDebug() << " decode error";
                return QByteArray();
            }
            int uncompressedBytes = outBuffer.size() - filter->outBufferAvailable();

            // append the uncompressed data to inflate buffer
            inflatedData.append(outBuffer.data(), uncompressedBytes);

            if (result == KFilterBase::End) {
                break; // Finished.
            }
        }

        if (result != KFilterBase::End && !filter->inBufferEmpty()) {
            qDebug() << "decode failed result" << result;
            delete filter;
            return QByteArray();
        }

        filter->terminate();
        delete filter;

        if (folder->unpackCRCDefined) {
            quint32 crc = crc32(0, (Bytef*)(inflatedData.data()),unpackSize);
            if (crc != folder->unpackCRC) {
                qDebug() << "wrong crc";
                return QByteArray();
            }
        }

        for (int j = 0; j < folder->packedStreams.size(); j++)
        {
            quint64 packSize = packSizes[packIndex++];
            pos += packSize;
            headerSize += packSize;
        }
    }
    return inflatedData;
}

///////////////// Write ////////////////////

void K7Zip::K7ZipPrivate::writeByte(unsigned char b)
{
    header.append(b);
    countSize++;
}

void K7Zip::K7ZipPrivate::writeNumber(quint64 value)
{
    int firstByte = 0;
    short mask = 0x80;
    int i;
    for (i = 0; i < 8; i++)
    {
        if (value < ((quint64(1) << ( 7  * (i + 1)))))
        {
            firstByte |= (int)(value >> (8 * i));
        break;
        }
        firstByte |= mask;
        mask >>= 1;
    }
    writeByte(firstByte);
    for (;i > 0; i--)
    {
        writeByte((int)value);
        value >>= 8;
    }
}

void K7Zip::K7ZipPrivate::writeBoolVector(const QVector<bool> &boolVector)
{
    int b = 0;
    short mask = 0x80;
    for (int i = 0; i < boolVector.size(); i++) {
        if (boolVector[i]) {
            b |= mask;
        }
        mask >>= 1;
        if (mask == 0) {
            writeByte(b);
            mask = 0x80;
            b = 0;
        }
    }
    if (mask != 0x80)
        writeByte(b);
}

void K7Zip::K7ZipPrivate::writeUInt32(quint32 value)
{
    for (int i = 0; i < 4; i++)
    {
        writeByte((unsigned char)value);
        value >>= 8;
    }
}

void K7Zip::K7ZipPrivate::writeUInt64(quint64 value)
{
    for (int i = 0; i < 8; i++) {
        printf("%02x", (unsigned char)value);
        writeByte((unsigned char)value);
        value >>= 8;
    }
    printf("\n");
}

void K7Zip::K7ZipPrivate::writeAlignedBoolHeader(const QVector<bool> &v, int numDefined, int type, unsigned itemSize)
{
    const unsigned bvSize = (numDefined == v.size()) ? 0 : ((unsigned)v.size() + 7) / 8;
    const quint64 dataSize = (quint64)numDefined * itemSize + bvSize + 2;
    //SkipAlign(3 + (unsigned)bvSize + (unsigned)GetBigNumberSize(dataSize), itemSize);

    writeByte(type);
    writeNumber(dataSize);
    if (numDefined == v.size()) {
        writeByte(1);
    } else {
        writeByte(0);
        writeBoolVector(v);
    }
    writeByte(0);
}

void K7Zip::K7ZipPrivate::writeUInt64DefVector(const QVector<quint64> &v, const QVector<bool> defined, int type)
{
    int numDefined = 0;

    for (int i = 0; i < defined.size(); i++) {
        if (defined[i]) {
            numDefined++;
        }
    }

    if (numDefined == 0)
        return;

    writeAlignedBoolHeader(defined, numDefined, type, 8);

    for (int i = 0; i < defined.size(); i++) {
        if (defined[i]) {
            writeUInt64(v[i]);
        }
    }
}

void K7Zip::K7ZipPrivate::writeHashDigests(
    const QVector<bool> &digestsDefined,
    const QVector<quint32> &digests)
{
    int numDefined = 0;
    int i;
    for (i = 0; i < digestsDefined.size(); i++) {
        if (digestsDefined[i]) {
            numDefined++;
        }
    }

    if (numDefined == 0) {
        return;
    }

    writeByte(kCRC);
    if (numDefined == digestsDefined.size()) {
        writeByte(1);
    } else {
        writeByte(0);
        writeBoolVector(digestsDefined);
    }

    for (i = 0; i < digests.size(); i++) {
        if (digestsDefined[i]) {
            writeUInt32(digests[i]);
        }
    }
}

void K7Zip::K7ZipPrivate::writePackInfo(quint64 dataOffset, QVector<quint64> &packedSizes, QVector<bool> &packedCRCsDefined, QVector<quint32> &packedCRCs)
{
    if (packedSizes.isEmpty())
        return;
    writeByte(kPackInfo);
    writeNumber(dataOffset);
    writeNumber(packedSizes.size());
    writeByte(kSize);

    for (int i = 0; i < packedSizes.size(); i++) {
        writeNumber(packedSizes[i]);
    }

    writeHashDigests(packedCRCsDefined, packedCRCs);

    writeByte(kEnd);
}

void K7Zip::K7ZipPrivate::writeFolder(const Folder *folder)
{
    writeNumber(folder->folderInfos.size());
    for (int i = 0; i < folder->folderInfos.size(); i++) {
        const Folder::FolderInfo *info = folder->folderInfos[i];
        {
            size_t propsSize = info->properties.size();

            quint64 id = info->methodID;
            size_t idSize;
            for (idSize = 1; idSize < sizeof(id); idSize++) {
                if ((id >> (8 * idSize)) == 0) {
                    break;
                }
            }

            int longID[15];
            for (int t = idSize - 1; t >= 0 ; t--, id >>= 8) {
                longID[t] = (int)(id & 0xFF);
            }

            int b;
            b = (int)(idSize & 0xF);
            bool isComplex = !info->isSimpleCoder();
            b |= (isComplex ? 0x10 : 0);
            b |= ((propsSize != 0) ? 0x20 : 0 );

            writeByte(b);
            for (size_t j = 0; j < idSize; ++j) {
                writeByte(longID[j]);
            }

            if (isComplex) {
                writeNumber(info->numInStreams);
                writeNumber(info->numOutStreams);
            }

            if (propsSize == 0)
                continue;

            writeNumber(propsSize);
            for (size_t j = 0; j < propsSize; ++j) {
                writeByte(info->properties[j]);
            }
        }
    }

    for (int i = 0; i < folder->inIndexes.size(); i++) {
        writeNumber(folder->inIndexes[i]);
        writeNumber(folder->outIndexes[i]);
    }

    if (folder->packedStreams.size() > 1) {
        for (int i = 0; i < folder->packedStreams.size(); i++) {
            writeNumber(folder->packedStreams[i]);
        }
    }
}

void K7Zip::K7ZipPrivate::writeUnpackInfo(QVector<Folder*> &folderItems)
{
    if (folderItems.isEmpty())
        return;

    writeByte(kUnpackInfo);

    writeByte(kFolder);
    writeNumber(folderItems.size());
    {
        writeByte(0);
        for (int i = 0; i < folderItems.size(); i++) {
            writeFolder(folderItems[i]);
        }
    }

    writeByte(kCodersUnpackSize);
    int i;
    for (i = 0; i < folderItems.size(); i++) {
        const Folder *folder = folderItems[i];
        for (int j = 0; j < folder->unpackSizes.size(); j++) {
            writeNumber(folder->unpackSizes[j]);
        }
    }

    QVector<bool> unpackCRCsDefined;
    QVector<quint32> unpackCRCs;
    for (i = 0; i < folderItems.size(); i++) {
        const Folder *folder = folderItems[i];
        unpackCRCsDefined.append(folder->unpackCRCDefined);
        unpackCRCs.append(folder->unpackCRC);
    }
    writeHashDigests(unpackCRCsDefined, unpackCRCs);

    writeByte(kEnd);
}

void K7Zip::K7ZipPrivate::writeSubStreamsInfo(
    const QVector<quint64> &unpackSizes,
    const QVector<bool> &digestsDefined,
    const QVector<quint32> &digests)
{
    writeByte(kSubStreamsInfo);

    for (int i = 0; i < numUnpackStreamsInFolders.size(); i++) {
        if (numUnpackStreamsInFolders[i] != 1) {
            writeByte(kNumUnpackStream);
            for (int j = 0; j < numUnpackStreamsInFolders.size(); j++) {
                writeNumber(numUnpackStreamsInFolders[j]);
            }
            break;
        }
    }

    bool needFlag = true;
    int index = 0;
    for (int i = 0; i < numUnpackStreamsInFolders.size(); i++) {
        for (quint32 j = 0; j < numUnpackStreamsInFolders[i]; j++)
        {
            if (j + 1 != numUnpackStreamsInFolders[i])
            {
                if (needFlag) {
                    writeByte(kSize);
                }
                needFlag = false;
                writeNumber(unpackSizes[index]);
            }
            index++;
        }
    }

    QVector<bool> digestsDefined2;
    QVector<quint32> digests2;

    int digestIndex = 0;
    for (int i = 0; i < folders.size(); i++)
    {
        int numSubStreams = (int)numUnpackStreamsInFolders[i];
        if (numSubStreams == 1 && folders[i]->unpackCRCDefined) {
            digestIndex++;
        } else {
            for (int j = 0; j < numSubStreams; j++, digestIndex++) {
                digestsDefined2.append(digestsDefined[digestIndex]);
                digests2.append(digests[digestIndex]);
            }
        }
    }
    writeHashDigests(digestsDefined2, digests2);
    writeByte(kEnd);
}

QByteArray K7Zip::K7ZipPrivate::encodeStream(QVector<quint64> &packSizes, QVector<Folder*> &folds)
{
    Folder *folder = new Folder;
    folder->unpackCRCDefined = true;
    folder->unpackCRC = crc32(0, (Bytef*)(header.data()), header.size());
    folder->unpackSizes.append(header.size());

    Folder::FolderInfo *info = new Folder::FolderInfo();
    info->numInStreams = 1;
    info->numOutStreams = 1;
    info->methodID = k_LZMA2;

    quint32 dictSize = header.size();
    const quint32 kMinReduceSize = (1 << 16);
    if (dictSize < kMinReduceSize) {
        dictSize = kMinReduceSize;
    }

    int dict;
    for (dict = 0; dict < 40; dict++) {
        if (dictSize <= LZMA2_DIC_SIZE_FROM_PROP(dict)) {
            break;
        }
    }

    info->properties.append(dict);
    folder->folderInfos.append(info);

    folds.append(folder);

    //compress data
    QBuffer* out = new QBuffer();
    out->open(QBuffer::ReadWrite);
    KCompressionDevice compression(out, true, KCompressionDevice::Xz);
    compression.open(QIODevice::WriteOnly);
    compression.write(header.data(), header.size());
    compression.close();

    QByteArray encodedData = out->data();

    // remove xz header + lzma2 header
    encodedData.remove(0, 6+18);
    // remove xz + lzma2 footer
    encodedData.remove(encodedData.size() - 29, 29);

    packSizes.append(encodedData.size());
    return encodedData;
}


void K7Zip::K7ZipPrivate::writeHeader(quint64 &headerOffset)
{
    quint64 packedSize = 0;
    for (int i=0; i < packSizes.size(); ++i) {
        packedSize += packSizes[i];
    }

    headerOffset = packedSize;

    writeByte(kHeader);

    // Archive Properties

    if (folders.size() > 0)
    {
        writeByte(kMainStreamsInfo);
        writePackInfo(0, packSizes, packCRCsDefined, packCRCs);

        writeUnpackInfo(folders);

        QVector<quint64> unpackFileSizes;
        QVector<bool> digestsDefined;
        QVector<quint32> digests;
        for (int i = 0; i < fileInfos.size(); i++) {
            const FileInfo *file = fileInfos[i];
            if (!file->hasStream)
                continue;
            unpackFileSizes.append(file->size);
            digestsDefined.append(file->crcDefined);
            digests.append(file->crc);
        }

        writeSubStreamsInfo(unpackSizes, digestsDefined, digests);
        writeByte(kEnd);
    }

    if (fileInfos.isEmpty()) {
        writeByte(kEnd);
        return;
    }

    writeByte(kFilesInfo);
    writeNumber(fileInfos.size());

    {
    /* ---------- Empty Streams ---------- */
        QVector<bool> emptyStreamVector;
        int numEmptyStreams = 0;
        for (int i = 0; i < fileInfos.size(); i++) {
            if (fileInfos[i]->hasStream) {
                emptyStreamVector.append(false);
            } else {
                emptyStreamVector.append(true);
                numEmptyStreams++;
            }
        }

        if (numEmptyStreams > 0) {
            writeByte(kEmptyStream);
            writeNumber(((unsigned)emptyStreamVector.size() + 7) / 8);
            writeBoolVector(emptyStreamVector);

            QVector<bool> emptyFileVector, antiVector;
            int numEmptyFiles = 0, numAntiItems = 0;
            for (int i = 0; i < fileInfos.size(); i++)
            {
                const FileInfo *file = fileInfos[i];
                if (!file->hasStream) {
                    emptyFileVector.append(!file->isDir);
                    if (!file->isDir) {
                        numEmptyFiles++;
                        bool isAnti = (i < this->isAnti.size() && this->isAnti[i]);
                        antiVector.append(isAnti);
                        if (isAnti) {
                            numAntiItems++;
                        }
                    }
                }
            }

            if (numEmptyFiles > 0) {
                writeByte(kEmptyFile);
                writeNumber(((unsigned)emptyFileVector.size() + 7) / 8);
                writeBoolVector(emptyFileVector);
            }

            if (numAntiItems > 0) {
                writeByte(kAnti);
                writeNumber(((unsigned)antiVector.size() + 7) / 8);
                writeBoolVector(antiVector);
            }
        }
    }

    {
        /* ---------- Names ---------- */

        int numDefined = 0;
        size_t namesDataSize = 0;
        for (int i = 0; i < fileInfos.size(); i++)
        {
            const QString &name = fileInfos[i]->path;
            if (!name.isEmpty()) {
                numDefined++;
                namesDataSize += (name.length() + 1) * 2;
            }
        }

        if (numDefined > 0) {
            namesDataSize++;
            //SkipAlign(2 + GetBigNumberSize(namesDataSize), 2);

            writeByte(kName);
            writeNumber(namesDataSize);
            writeByte(0);
            for (int i = 0; i < fileInfos.size(); i++) {
                const QString &name = fileInfos[i]->path;
                for (int t = 0; t < name.length(); t++)
                {
                    wchar_t c = name[t].toLatin1();
                    writeByte((unsigned char)c);
                    writeByte((unsigned char)(c >> 8));
                }
                // End of string
                writeByte(0);
                writeByte(0);
            }
        }
    }

    writeUInt64DefVector(mTimes, mTimesDefined, kMTime);

    writeUInt64DefVector(startPositions, startPositionsDefined, kStartPos);

    {
        /* ---------- Write Attrib ---------- */
        QVector<bool> boolVector;
        int numDefined = 0;
        for (int i = 0; i < fileInfos.size(); i++) {
            bool defined = fileInfos[i]->attribDefined;
            boolVector.append(defined);
            if (defined) {
                numDefined++;
            }
        }

        if (numDefined > 0) {
            writeAlignedBoolHeader(boolVector, numDefined, kAttributes, 4);
            for (int i = 0; i < fileInfos.size(); i++) {
                const FileInfo *file = fileInfos[i];
                if (file->attribDefined) {
                    writeUInt32(file->attributes);
                }
            }
        }
    }

    writeByte(kEnd); // for files
    writeByte(kEnd); // for headers*/
}

static void setUInt32(unsigned char *p, quint32 d)
{
    for (int i = 0; i < 4; i++, d >>= 8) {
        p[i] = (unsigned)d;
    }
}

static void setUInt64(unsigned char *p, quint64 d)
{
    for (int i = 0; i < 8; i++, d >>= 8) {
        p[i] = (unsigned char)d;
    }
}

void K7Zip::K7ZipPrivate::writeStartHeader(const quint64 nextHeaderSize, const quint32 nextHeaderCRC, const quint64 nextHeaderOffset)
{
    unsigned char buf[24];
    setUInt64(buf + 4, nextHeaderOffset);
    setUInt64(buf + 12, nextHeaderSize);
    setUInt32(buf + 20, nextHeaderCRC);
    setUInt32(buf, crc32(0, (Bytef*)(buf + 4), 20));
    q->device()->write((char*)buf, 24);
}

void K7Zip::K7ZipPrivate::writeSignature()
{
    unsigned char buf[8];
    memcpy(buf, k7zip_signature, 6);
    buf[6] = 0/*kMajorVersion*/;
    buf[7] = 3;
    q->device()->write((char*)buf, 8);
}

bool K7Zip::openArchive( QIODevice::OpenMode mode )
{
    if ( !(mode & QIODevice::ReadOnly) )
        return true;

    QIODevice* dev = device();

    char header[32];

    if ( !dev )
        return false;

    // check signature
    qint64 n = dev->read( header, 32 );
    if (n != 32) {
        qDebug() << "read header failed";
        return false;
    }

    for ( int i = 0; i < 6; ++i ) {
        if ( (unsigned char)header[i] != k7zip_signature[i] ) {
            qDebug() << "check signature failed";
            return false;
        }
    }

    // get Archive Version
    int major = header[6];
    int minor = header[7];

    /*if (major > 0 || minor > 2) {
        qDebug() << "wrong archive version";
        return false;
    }*/

    // get Start Header CRC
    quint32 startHeaderCRC = GetUi32(header, 8);
    quint64 nextHeaderOffset = GetUi64(header, 12);
    quint64 nextHeaderSize = GetUi64(header, 20);
    quint32 nextHeaderCRC = GetUi32(header, 28);

    quint32 crc = crc32(0, (Bytef*)(header + 0xC), 20);

    if (crc != startHeaderCRC) {
        qDebug() << "bad crc";
        return false;
    }

    if (nextHeaderSize == 0) {
        return true;
    }

    if (nextHeaderSize > (quint64)0xFFFFFFFF) {
        return false;
    }

    if ((qint64)nextHeaderOffset < 0) {
        return false;
    }

    dev->seek(nextHeaderOffset + 32);

    QByteArray inBuffer;
    inBuffer.resize(nextHeaderSize);

    n = dev->read(inBuffer.data(), inBuffer.size());
    if ( n != (qint64)nextHeaderSize ) {
        qDebug() << "Failed read next header size, should read " << nextHeaderSize << ", read " << n;
        return false;
    }
    d->buffer = inBuffer.data();
    d->end = nextHeaderSize;

    d->headerSize = 32 + nextHeaderSize;
    //int physSize = 32 + nextHeaderSize + nextHeaderOffset;

    crc = crc32(0, (Bytef*)(d->buffer), (quint32)nextHeaderSize);

    if (crc != nextHeaderCRC) {
        qDebug() << "bad next header crc";
        return false;
    }

    int type = d->readByte();
    if (type != kHeader) {
        if (type != kEncodedHeader) {
            qDebug() << "error in header";
            return false;
        }
        QByteArray decodedData = d->readAndDecodePackedStreams();

        QByteArray newHeader;
        newHeader.resize(d->headerSize);

        dev->seek(d->pos);
        quint64 n = dev->read(newHeader.data(), newHeader.size());
        if ( n != (qint64) d->headerSize) {
            qDebug() << "Failed read new header size, should read " << newHeader.size() << ", read " << n;
            return false;
        }
        d->pos = 0;
        d->end = d->headerSize;

        int external = d->readByte();
        if (external != 0) {
            int dataIndex = (int)d->readNumber();
            if (dataIndex < 0 /*|| dataIndex >= dataVector->Size()*/) {
                qDebug() << "dataIndex error";
            }
            d->buffer = decodedData.data();
            d->pos = 0;
            d->end = decodedData.size();
        }

        type = d->readByte();
        if (type != kHeader) {
            qDebug() << "error type should be kHeader";
            return false;
        }
    }
    // read header
    type = d->readByte();

    if (type == kArchiveProperties) {
        // TODO : implement this part
        qDebug() << "not implemented";
        return false;
    }

    if (type == kAdditionalStreamsInfo) {
        // TODO : implement this part
        qDebug() << "not implemented";
        return false;
    }

    if (type == kMainStreamsInfo) {
        if (!d->readMainStreamsInfo()) {
            qDebug() << "error during read main streams information";
            return false;
        }
        type = d->readByte();
    } else {
        for (int i = 0; i < d->folders.size(); ++i)
        {
            Folder* folder = d->folders[i];
            quint64 unpackSize = 0;

            if (!folder->unpackSizes.isEmpty()) {
                for (int j = folder->unpackSizes.size() - 1; j >= 0; j--) {
                    for(int k = 0; k < folder->outIndexes.size(); k++) {
                        if (folder->outIndexes[k] == folder->unpackSizes[j]) {
                            unpackSize = folder->unpackSizes[j];
                        }
                    }
                }
            }
            d->unpackSizes.append(unpackSize);
            d->digestsDefined.append(folder->unpackCRCDefined);
            d->digests.append(folder->unpackCRC);
        }
    }

    if (type == kEnd) {
        return true;
    }
    if (type != kFilesInfo) {
        qDebug() << "read header error";
        return false;
    }

    //read files info
    int numFiles = d->readNumber();
    for (int i=0; i < numFiles; ++i) {
        d->fileInfos.append(new FileInfo);
    }

    QVector<bool> emptyStreamVector;
    QVector<bool> emptyFileVector;
    QVector<bool> antiFileVector;
    int numEmptyStreams = 0;

    for (;;)
    {
        quint64 type = d->readByte();
        if (type == kEnd)
            break;

        quint64 size = d->readNumber();

        size_t ppp = d->pos;

        bool addPropIdToList = true;
        bool isKnownType = true;

        if (type > ((quint32)1 << 30)) {
            isKnownType = false;
        } else {
            switch(type)
            {
            case kEmptyStream:
            {
                d->readBoolVector(numFiles, emptyStreamVector);
                for (int i = 0; i < emptyStreamVector.size(); ++i) {
                    if (emptyStreamVector[i]) {
                        numEmptyStreams++;
                    }
                }

                break;
            }
            case kEmptyFile:
                d->readBoolVector(numEmptyStreams, emptyFileVector);
                break;
            case kAnti:
                d->readBoolVector(numEmptyStreams, antiFileVector);
                break;
            case kCTime:
                if(!d->readUInt64DefVector(numFiles, d->cTimes, d->cTimesDefined)) {
                    qDebug() << "error read CTime";
                    return false;
                }
                break;
            case kATime:
                if(!d->readUInt64DefVector(numFiles, d->aTimes, d->aTimesDefined)) {
                    qDebug() << "error read ATime";
                    return false;
                }
                break;
            case kMTime:
                if(!d->readUInt64DefVector(numFiles, d->mTimes, d->mTimesDefined)) {
                    qDebug() << "error read MTime";
                    return false;
                }
                break;
            case kName:
            {
                int external = d->readByte();
                if (external != 0) {
                    int dataIndex = d->readNumber();
                    if (dataIndex < 0 /*|| dataIndex >= dataVector->Size()*/) {
                       qDebug() << "wrong data index";
                    }

                    // TODO : go to the new index
                }

                QString name;
                for (int i = 0; i < numFiles; i++) {
                    name = d->readString();
                    d->fileInfos[i]->path = name;
                }
                break;
            }
            case kAttributes:
            {
                QVector<bool> attributesAreDefined;
                d->readBoolVector2(numFiles, attributesAreDefined);
                int external = d->readByte();
                if (external != 0) {
                    int dataIndex = d->readNumber();
                    if (dataIndex < 0) {
                        qDebug() << "wrong data index";
                    }

                    // TODO : go to the new index
                }

                for (int i = 0; i < numFiles; i++)
                {
                    FileInfo* fileInfo = d->fileInfos[i];
                    fileInfo->attribDefined = attributesAreDefined[i];
                    if (fileInfo->attribDefined) {
                        fileInfo->attributes = d->readUInt32();
                    }
                }
                break;
            }
            case kStartPos:
                if(!d->readUInt64DefVector(numFiles, d->startPositions, d->startPositionsDefined)) {
                    qDebug() << "error read MTime";
                    return false;
                }
                break;
            case kDummy:
            {
                for (quint64 i = 0; i < size; i++) {
                    if (d->readByte() != 0) {
                        qDebug() << "invalid";
                        return false;
                    }
                }
                addPropIdToList = false;
                break;
            }
            default:
                addPropIdToList = isKnownType = false;
            }
        }

        if (isKnownType) {
            if(addPropIdToList) {
                d->fileInfoPopIDs.append(type);
            }
        } else {
            d->skipData(d->readNumber());
        }

        bool checkRecordsSize = (major > 0 ||
                                 minor > 2);
        if (checkRecordsSize && d->pos - ppp != size) {
            qDebug() << "error read size failed";
            return false;
        }
    }

    int emptyFileIndex = 0;
    int sizeIndex = 0;

    int numAntiItems = 0;

    if (emptyStreamVector.isEmpty()) {
        for (int i = 0; i < numFiles; ++i) {
            emptyStreamVector.append(false);
        }
    }

    if (antiFileVector.isEmpty()) {
        for (int i = 0; i < numEmptyStreams; i++) {
            antiFileVector.append(false);
        }
    }
    if (emptyFileVector.isEmpty()) {
        for (int i = 0; i < numEmptyStreams; i++) {
            emptyFileVector.append(false);
        }
    }

    for (int i = 0; i < numEmptyStreams; i++) {
        if (antiFileVector[i]) {
            numAntiItems++;
        }
    }

    int oldPos = 0;
    for (int i = 0; i < numFiles; i++)
    {
        FileInfo* fileInfo = d->fileInfos[i];
        bool isAnti;
        fileInfo->hasStream = !emptyStreamVector[i];
        if (fileInfo->hasStream)
        {
            fileInfo->isDir = false;
            isAnti = false;
            fileInfo->size = d->unpackSizes[sizeIndex];
            fileInfo->crc = d->digests[sizeIndex];
            fileInfo->crcDefined = d->digestsDefined[sizeIndex];
            sizeIndex++;
        } else {
            fileInfo->isDir = !emptyFileVector[emptyFileIndex];
            isAnti = antiFileVector[emptyFileIndex];
            emptyFileIndex++;
            fileInfo->size = 0;
            fileInfo->crcDefined = false;
        }
        if (numAntiItems != 0)
            d->isAnti.append(isAnti);

        QString attr;
        if ((fileInfo->attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 || fileInfo->isDir)
            attr += QLatin1String("D");
        if ((fileInfo->attributes & FILE_ATTRIBUTE_READONLY) != 0)
            attr += QLatin1String("R");
        if ((fileInfo->attributes & FILE_ATTRIBUTE_HIDDEN) != 0)
            attr += QLatin1String("H");
        if ((fileInfo->attributes & FILE_ATTRIBUTE_SYSTEM) != 0)
            attr += QLatin1String("S");
        if ((fileInfo->attributes & FILE_ATTRIBUTE_ARCHIVE) != 0)
            attr += QLatin1String("A");

        quint64 packSize = 0;
        for (int j = 0; j < d->packSizes.size(); j++) {
            packSize += d->packSizes[j];
        }
        //unsigned short st_mode =  fileInfo->attributes >> 16;
        QString method;
        for (int w = 0; w < d->folders.size(); ++w) {
            for (int g = 0; g < d->folders[w]->folderInfos.size(); ++g) {
                Folder::FolderInfo* info = d->folders[w]->folderInfos[g];
                switch(info->methodID) {
                case k_LZMA:
                    method = QLatin1String("LZMA:16");
                    break;
                case k_LZMA2:
                    method = QLatin1String("LZMA2");
                    break;
                case k_AES:
                    break;
                }
            }
        }

        int access;
        if (fileInfo->attributes & FILE_ATTRIBUTE_UNIX_EXTENSION) {
            access = fileInfo->attributes >> 16;
            if (S_ISLNK(access)) {
                // TODO : Implement this part
                qDebug() << "find symlink";
            }
        } else {
            if (fileInfo->isDir) {
                access = S_IFDIR | 0755;
            } else {
                access = 0100644;
            }
        }

        qint64 pos = i == 0 ? 0 : pos + oldPos;
        if (!fileInfo->isDir) {
            oldPos = fileInfo->size;
        }

        KArchiveEntry* e;
        QString entryName;
        int index = fileInfo->path.lastIndexOf(QLatin1Char('/'));
        if ( index == -1 ) {
            entryName = fileInfo->path;
        } else {
            entryName = fileInfo->path.mid( index + 1 );
        }
        Q_ASSERT( !entryName.isEmpty() );

        time_t mTime;
        if (d->mTimesDefined[i]) {
            mTime = toTimeT(d->mTimes[i]);
        } else {
            mTime = time(NULL);
        }

        if (fileInfo->isDir) {
            QString path = QDir::cleanPath( fileInfo->path );
            const KArchiveEntry* ent = rootDir()->entry( path );
            if ( ent && ent->isDirectory() ) {
                e = 0;
            } else {
                e = new KArchiveDirectory( this, entryName, access, mTime, rootDir()->user(), rootDir()->group(), QString()/*symlink*/ );
            }
        } else {
            e = new KArchiveFile( this, entryName, access, mTime, rootDir()->user(), rootDir()->group(), QString()/*symlink*/, pos, fileInfo->size );
        }

        if (e) {
            if (index == -1) {
                rootDir()->addEntry( e );
            } else {
                QString path = QDir::cleanPath( fileInfo->path.left( index ) );
                KArchiveDirectory * d = findOrCreate( path );
                d->addEntry( e );
            }
        }
    }

    QByteArray decodedData = d->readAndDecodePackedStreams(false);
    if (decodedData.isEmpty()) {
        return false;
    }

    QBuffer* out = new QBuffer();
    QByteArray* array = new QByteArray();
    array->append(decodedData);
    out->setBuffer(array);
    out->open(QIODevice::ReadOnly);
    setDevice(out);

    return true;
}

bool K7Zip::closeArchive()
{
    if ( !isOpen() )
    {
        //qWarning() << "You must open the file before close it\n";
        return false;
    }

    if (mode() & QIODevice::ReadOnly)
    {
        return true;
    }

    //compress data
    QBuffer* out = new QBuffer();
    out->open(QBuffer::ReadWrite);
    KCompressionDevice compression(out, true, KCompressionDevice::Xz);
    compression.open(QIODevice::WriteOnly);
    compression.write(d->outData.data(), d->outData.size());
    compression.close();

    QByteArray encodedData = out->data();

    // remove xz header + lzma2 header
    encodedData.remove(0, 6+18);
    // remove xz + lzma2 footer
    encodedData.remove(encodedData.size() - 29, 29);

    d->packSizes.append(encodedData.size());

    Folder *folder = new Folder();
    folder->unpackCRCDefined = true;
    folder->unpackCRC = crc32(0, (Bytef*)(d->outData.data()), d->outData.size());
    folder->unpackSizes.append(d->outData.size());

    Folder::FolderInfo *info = new Folder::FolderInfo();
    info->numInStreams = 1;
    info->numOutStreams = 1;
    info->methodID = k_LZMA2;

    quint32 dictSize = d->outData.size();

    const quint32 kMinReduceSize = (1 << 16);
    if (dictSize < kMinReduceSize) {
        dictSize = kMinReduceSize;
    }

    // k_LZMA2 mehtod
    int dict;
    for (dict = 0; dict < 40; dict++) {
        if (dictSize <= LZMA2_DIC_SIZE_FROM_PROP(dict)) {
            break;
        }
    }
    info->properties.append(dict);

    folder->folderInfos.append(info);
    d->folders.append(folder);

    d->numUnpackStreamsInFolders.append(d->fileInfos.size());

    quint64 headerOffset;
    d->writeHeader(headerOffset);

    // Encode Header
    d->writeHeader(headerOffset);

    QVector<quint64> packSizes;
    QVector<Folder*> folders;
    QByteArray encodedStream = d->encodeStream(packSizes, folders);

    if (folders.isEmpty()) {
        return false;
    }

    d->writeByte(kEncodedHeader);
    QVector<bool> emptyDefined;
    QVector<quint32> emptyCrcs;
    d->writePackInfo(headerOffset, packSizes, emptyDefined , emptyCrcs);
    d->writeUnpackInfo(folders);
    d->writeByte(kEnd);
    for (int i = 0; i < packSizes.size(); i++) {
        headerOffset += packSizes[i];
    }


    quint64 nextHeaderSize = d->header.size();
    quint32 nextHeaderCRC = crc32(0, (Bytef*)(d->header.data()), d->header.size());
    quint64 nextHeaderOffset = headerOffset;

    d->writeSignature();
    d->writeStartHeader(nextHeaderSize, nextHeaderCRC, nextHeaderOffset);
    device()->write(encodedData.data(), encodedData.size());
    device()->write(encodedStream.data(), encodedStream.size());
    device()->write(d->header.data(), d->header.size());

    return true;
}

bool K7Zip::doFinishWriting( qint64 /*size*/ ) {
    return true;
}

bool K7Zip::writeData(const char * data, qint64 size)
{
    FileInfo *info = d->fileInfos.last();
    info->size = size;
    info->hasStream = true;
    d->outData.append(data, size);
    d->unpackSizes.append(size);

    return true;
}

bool K7Zip::doPrepareWriting(const QString &name, const QString &/*user*/,
                          const QString &/*group*/, qint64 /*size*/, mode_t perm,
                          time_t atime, time_t mtime, time_t ctime)
{
    if ( !isOpen() )
    {
        //qWarning() << "You must open the tar file before writing to it\n";
        return false;
    }

    if ( !(mode() & QIODevice::WriteOnly) )
    {
        //qWarning() << "You must open the tar file for writing\n";
        return false;
    }

    // In some files we can find dir/./file => call cleanPath
    QString fileName ( QDir::cleanPath( name ) );

    FileInfo *fileInfo = new FileInfo;
    fileInfo->path = fileName;
    fileInfo->attribDefined = true;
    fileInfo->attributes = FILE_ATTRIBUTE_ARCHIVE;
    fileInfo->attributes |= FILE_ATTRIBUTE_UNIX_EXTENSION + ((perm & 0xFFFF) << 16);

    d->cTimesDefined.append(true);
    d->mTimesDefined.append(true);
    d->aTimesDefined.append(true);
    d->cTimes.append(rtlSecondsSince1970ToSpecTime(ctime));
    d->mTimes.append(rtlSecondsSince1970ToSpecTime(mtime));
    d->aTimes.append(rtlSecondsSince1970ToSpecTime(atime));

    d->fileInfos.append(fileInfo);

    return true;
}

bool K7Zip::doWriteDir(const QString &name, const QString &/*user*/,
                      const QString &/*group*/, mode_t perm,
                      time_t atime, time_t mtime, time_t ctime)
{
    if ( !isOpen() )
    {
        //qWarning() << "You must open the tar file before writing to it\n";
        return false;
    }

    if ( !(mode() & QIODevice::WriteOnly) )
    {
        //qWarning() << "You must open the tar file for writing\n";
        return false;
    }

    // In some tar files we can find dir/./ => call cleanPath
    QString dirName ( QDir::cleanPath( name ) );

    // Remove trailing '/'
    if ( dirName.endsWith( QLatin1Char( '/' ) ) )
        dirName.remove(dirName.size() - 1, 1);

    FileInfo *fileInfo = new FileInfo;
    fileInfo->path = dirName;
    fileInfo->attributes = FILE_ATTRIBUTE_DIRECTORY;
    fileInfo->attributes |= FILE_ATTRIBUTE_UNIX_EXTENSION + ((perm & 0xFFFF) << 16);

    d->cTimes.append(rtlSecondsSince1970ToSpecTime(ctime));
    d->mTimes.append(rtlSecondsSince1970ToSpecTime(mtime));
    d->aTimes.append(rtlSecondsSince1970ToSpecTime(atime));

    d->fileInfos.append(fileInfo);

    return true;
}

bool K7Zip::doWriteSymLink(const QString &name, const QString &target,
                        const QString &/*user*/, const QString &/*group*/,
                        mode_t perm, time_t atime, time_t mtime, time_t ctime)
{
    if ( !isOpen() )
    {
        //qWarning() << "You must open the tar file before writing to it\n";
        return false;
    }

    if ( !(mode() & QIODevice::WriteOnly) )
    {
        //qWarning() << "You must open the tar file for writing\n";
        return false;
    }

    // In some files we can find dir/./file => call cleanPath
    QString fileName ( QDir::cleanPath( name ) );
    QByteArray encodedTarget = QFile::encodeName(target);

    FileInfo *fileInfo = new FileInfo;
    fileInfo->path = fileName;
    fileInfo->attributes = FILE_ATTRIBUTE_ARCHIVE;
    fileInfo->attributes |= FILE_ATTRIBUTE_UNIX_EXTENSION + ((perm & 0xFFFF) << 16);

    d->cTimes.append(rtlSecondsSince1970ToSpecTime(ctime));
    d->mTimes.append(rtlSecondsSince1970ToSpecTime(mtime));
    d->aTimes.append(rtlSecondsSince1970ToSpecTime(atime));

    d->fileInfos.append(fileInfo);

    return true;
}

void K7Zip::virtual_hook( int id, void* data ) {
    KArchive::virtual_hook( id, data );
}