// ArHandler.cpp

#include "StdAfx.h"

#include <stdio.h>

#include "../../../../C/CpuArch.h"

#include "../../../../C/7zCrc.h"

#include "../../../Common/ComTry.h"
#include "../../../Common/IntToString.h"
#include "../../../Common/StringConvert.h"
#include "../../../Common/StringToInt.h"

#include "../../../Windows/PropVariant.h"
#include "../../../Windows/TimeUtils.h"

#include "../../Common/LimitedStreams.h"
#include "../../Common/ProgressUtils.h"
#include "../../Common/RegisterArc.h"
#include "../../Common/StreamObjects.h"
#include "../../Common/StreamUtils.h"

#include "../../Compress/CopyCoder.h"
#include "../../Compress/BZip2Crc.h"

#include "../Common/ItemNameUtils.h"

using namespace NWindows;
using namespace NTime;

namespace NArchive {
    namespace NUmod {
    	
        struct CHeader
        {
            unsigned Signature;
            unsigned DirOffset;
            unsigned TotalBytes;
            unsigned FileVersion;
            unsigned CRC32;
        };
    	
        static const unsigned kSignature = 0x9FE3C5A3;

        struct CItemInfo
        {
            unsigned Offset;
            unsigned Size;
            unsigned flags;
        };

        struct CItem
        {
            AString Name;
            CItemInfo Info;
        };

        class CInArchive
        {
            CMyComPtr<IInStream> m_Stream;

        public:
            UInt64 Position;
            UInt64 ItemsCount;
            CHeader Header = {};

            HRESULT GetNextItem(CItem& itemInfo);
            HRESULT Open(IInStream* inStream);
            HRESULT ReadIndex(UInt64& output);
        };

        HRESULT CInArchive::ReadIndex(UInt64& output)
        { // https://beyondunrealwiki.github.io/pages/package-file-format-data-de.html
            output = 0;
            for (int i = 0; i < 5; i++)
            {
                byte x = 0;
                RINOK(ReadStream_FALSE(m_Stream, &x, sizeof(x)));
                // First byte
                if (i == 0)
                {
                    // Bit: X0000000
                    if ((x & 0x80) > 0)
                        return S_FALSE; // not support negative values
                    // Bits: 00XXXXXX
                    output |= (x & 0x3F);
                    // Bit: 0X000000
                    if ((x & 0x40) == 0)
                        break;
                }
                // Last byte
                else if (i == 4)
                {
                    // Bits: 000XXXXX -- the 0 bits are ignored
                    // (hits the 32 bit boundary)
                    output |= (x & 0x1F) << (6 + (3 * 7));
                }
                // Middle bytes
                else
                {
                    // Bits: 0XXXXXXX
                    output |= (x & 0x7F) << (6 + ((i - 1) * 7));
                    // Bit: X0000000
                    if ((x & 0x80) == 0)
                        break;
                }
            }
            return S_OK;
        }

        HRESULT CInArchive::Open(IInStream* inStream)
        {
            RINOK(inStream->Seek(0-sizeof(Header), STREAM_SEEK_END, NULL));
            RINOK(ReadStream_FALSE(inStream, &Header, sizeof(Header)));
            if (Header.Signature != kSignature)
                return S_FALSE;            
            m_Stream = inStream;
            RINOK(inStream->Seek(Header.DirOffset, STREAM_SEEK_SET, NULL));
            RINOK(ReadIndex(ItemsCount));
            Position = 0;
            return S_OK;
        }

        HRESULT CInArchive::GetNextItem(CItem& item)
        {
            UInt64 Len = 0;
            RINOK(ReadIndex(Len));

            char* s = item.Name.GetBuf((unsigned)Len);
            size_t processedSize = Len;
            HRESULT res = ReadStream(m_Stream, s, &processedSize);
            item.Name.ReleaseBuf_CalcLen((unsigned)Len);
            RINOK(res);

            processedSize = sizeof(item.Info);
            RINOK(ReadStream(m_Stream, &item.Info, &processedSize));

            Position++;
            return S_OK;
        }

        class CHandler :
            public IInArchive,
            public IInArchiveGetStream,
            public CMyUnknownImp
        {
            CObjectVector<CItem> _items;
            CMyComPtr<IInStream> _stream;
            CHeader Header = {};

            AString _errorMessage;

            void UpdateErrorMessage(const char* s);
        public:
            MY_UNKNOWN_IMP2(IInArchive, IInArchiveGetStream)
                INTERFACE_IInArchive(;)
                STDMETHOD(GetStream)(UInt32 index, ISequentialInStream** stream);
        };

        void CHandler::UpdateErrorMessage(const char* s)
        {
            if (!_errorMessage.IsEmpty())
                _errorMessage += '\n';
            _errorMessage += s;
        }

        static const Byte kArcProps[] =
        {
          kpidCRC,
		  kpidUnpackVer,
          kpidWarning
        };

        static const Byte kProps[] =
        {
          kpidPath,
          kpidOffset,
          kpidPackSize,
          kpidSize,
          kpidAttrib,
        };

        IMP_IInArchive_Props
        IMP_IInArchive_ArcProps

        STDMETHODIMP CHandler::Open(IInStream* stream,
            const UInt64* /* maxCheckStartPosition */,
            IArchiveOpenCallback* callback)
        {
            COM_TRY_BEGIN
            {
              Close();

              UInt64 fileSize = 0;
              RINOK(stream->Seek(0, STREAM_SEEK_END, &fileSize));
              RINOK(stream->Seek(0, STREAM_SEEK_SET, NULL));

              CInArchive arc;
              RINOK(arc.Open(stream));

              if (callback)
              {
                RINOK(callback->SetTotal(NULL, &arc.ItemsCount));
                UInt64 numFiles = _items.Size();
                RINOK(callback->SetCompleted(&numFiles, &arc.Position));
              }

              CItem item;
              for (int i = 0; i < arc.ItemsCount; i++)
              {
                RINOK(arc.GetNextItem(item));
                _items.Add(item);
                if (callback && (_items.Size() & 0xFF) == 0)
                {
                  UInt64 numFiles = _items.Size();
                  RINOK(callback->SetCompleted(&numFiles, &arc.Position));
                }
              }

              if (arc.Header.TotalBytes != fileSize)
                  UpdateErrorMessage("Header size mismatch with real file size");

              UInt64 HeaderOffset = fileSize - sizeof(CHeader);
              UInt64 Position = 0;
              UInt64 numFiles = _items.Size();
              if (callback)
              {
                  RINOK(callback->SetTotal(NULL, &HeaderOffset));
                  RINOK(callback->SetCompleted(&numFiles, &Position));
              }

              CBZip2Crc::InitTable();
              CBZip2Crc crc;
              RINOK(stream->Seek(0, STREAM_SEEK_SET, NULL));
				char Buf[16384];
				while(Position < HeaderOffset)
				{
                    UInt64 Read = sizeof(Buf);
                    if (Position + Read >= HeaderOffset)
                        Read = HeaderOffset - Position;
                    RINOK(ReadStream_FALSE(stream, &Buf, Read));
                    Position += Read;
                    for (UInt32 i = 0; i < Read; i++)
                        crc.UpdateByte(((const Byte*)Buf)[i]);
                    if (callback)
                    {
                        RINOK(callback->SetCompleted(&numFiles, &Position));
                    }
				}
                if (arc.Header.CRC32 != crc.GetDigest())
                {
                    sprintf_s(&Buf[0], sizeof(Buf), "Archive damaged: CRC not match: %X", crc.GetDigest());
                    UpdateErrorMessage(&Buf[0]);
                }

              _stream = stream;
              Header = arc.Header;
            }
            return S_OK;
            COM_TRY_END
        }

        STDMETHODIMP CHandler::Close()
        {
            Header = {};
            _errorMessage.Empty();
            _stream.Release();
            _items.Clear();

            return S_OK;
        }

        STDMETHODIMP CHandler::GetNumberOfItems(UInt32* numItems)
        {
            *numItems = _items.Size();
            return S_OK;
        }

        STDMETHODIMP CHandler::GetArchiveProperty(PROPID propID, PROPVARIANT* value)
        {
            COM_TRY_BEGIN
                NCOM::CPropVariant prop;
            switch (propID)
            {
            case kpidCRC: prop = Header.CRC32; break;
            case kpidUnpackVer:  prop = Header.FileVersion; break;
            case kpidWarning: if (!_errorMessage.IsEmpty()) prop = _errorMessage; break;
            }
            prop.Detach(value);
            return S_OK;
            COM_TRY_END
        }

        STDMETHODIMP CHandler::GetProperty(UInt32 index, PROPID propID, PROPVARIANT* value)
        {
            COM_TRY_BEGIN
                NWindows::NCOM::CPropVariant prop;
            const CItem& item = _items[index];
            switch (propID)
            {
            case kpidPath:
                prop = (const wchar_t*)NItemName::GetOsPath_Remove_TailSlash(MultiByteToUnicodeString(item.Name, CP_OEMCP));
                break;
            case kpidOffset:
                prop = item.Info.Offset;
                break;
            case kpidSize:
            case kpidPackSize:
                prop = item.Info.Size;
                break;
            case kpidAttrib:
                prop = item.Info.flags;
                break;
            }
            prop.Detach(value);
            return S_OK;
            COM_TRY_END
        }

        STDMETHODIMP CHandler::Extract(const UInt32* indices, UInt32 numItems,
            Int32 testMode, IArchiveExtractCallback* extractCallback)
        {
            COM_TRY_BEGIN
                bool allFilesMode = (numItems == (UInt32)(Int32)-1);
            if (allFilesMode)
                numItems = _items.Size();
            if (numItems == 0)
                return S_OK;
            UInt64 totalSize = 0;
            UInt32 i;
            for (i = 0; i < numItems; i++)
            {
                const CItem& item = _items[allFilesMode ? i : indices[i]];
                totalSize += item.Info.Size;
            }
            extractCallback->SetTotal(totalSize);

            UInt64 currentTotalSize = 0;

            NCompress::CCopyCoder* copyCoderSpec = new NCompress::CCopyCoder();
            CMyComPtr<ICompressCoder> copyCoder = copyCoderSpec;

            CLocalProgress* lps = new CLocalProgress;
            CMyComPtr<ICompressProgressInfo> progress = lps;
            lps->Init(extractCallback, false);

            CLimitedSequentialInStream* streamSpec = new CLimitedSequentialInStream;
            CMyComPtr<ISequentialInStream> inStream(streamSpec);
            streamSpec->SetStream(_stream);

            for (i = 0; i < numItems; i++)
            {
                lps->InSize = lps->OutSize = currentTotalSize;
                RINOK(lps->SetCur());
                CMyComPtr<ISequentialOutStream> realOutStream;
                Int32 askMode = testMode ?
                    NExtract::NAskMode::kTest :
                    NExtract::NAskMode::kExtract;
                Int32 index = allFilesMode ? i : indices[i];
                const CItem& item = _items[index];
                RINOK(extractCallback->GetStream(index, &realOutStream, askMode));
                currentTotalSize += item.Info.Size;

                if (!testMode && !realOutStream)
                    continue;
                RINOK(extractCallback->PrepareOperation(askMode));
                if (testMode)
                {
                    RINOK(extractCallback->SetOperationResult(NExtract::NOperationResult::kOK));
                    continue;
                }
                bool isOk = true;                
                RINOK(_stream->Seek(item.Info.Offset, STREAM_SEEK_SET, NULL));
                streamSpec->Init(item.Info.Size);
                RINOK(copyCoder->Code(inStream, realOutStream, NULL, NULL, progress));
                isOk = (copyCoderSpec->TotalSize == item.Info.Size);
                realOutStream.Release();
                RINOK(extractCallback->SetOperationResult(isOk ?
                    NExtract::NOperationResult::kOK :
                    NExtract::NOperationResult::kDataError));
            }
            return S_OK;
            COM_TRY_END
        }

        STDMETHODIMP CHandler::GetStream(UInt32 index, ISequentialInStream** stream)
        {
            COM_TRY_BEGIN
                const CItem& item = _items[index];
            return CreateLimitedInStream(_stream, item.Info.Offset, item.Info.Size, stream);
            COM_TRY_END
        }

        REGISTER_ARC_I_NO_SIG(
            "Umod", "Unreal install file", 0, 0xAA,
            0,
            0,
            NULL)
    }
}
