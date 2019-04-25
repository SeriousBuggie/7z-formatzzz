#include "stdafx.h"

#pragma warning (disable: 5043)

#include "../../../Common/ComTry.h"
#include "../../../Common/MyString.h"

#include "../../../Windows/PropVariant.h"

#include "../../Common/ProgressUtils.h"
#include "../../Common/RegisterArc.h"
#include "../../Common/StreamObjects.h"

#include <sstream>

using namespace NWindows;

namespace NArchive {
namespace NZzz {

static const Byte kProps[] =
{
	kpidPath,
	kpidSize,
	kpidPackSize
};

struct MyVirtualItem
{
	const wchar_t* name;
	const char* text;
} items[] = 
{
	{L"文件1.txt", "内容1"},
	{L"文件2.txt", "内容2"},
	{L"递归调用测试.zzz", "111"}
};

class CHandler :
	public IInArchive,
	public IInArchiveGetStream,
	public CMyUnknownImp
{
public:
	MY_UNKNOWN_IMP2(IInArchive, IInArchiveGetStream)
	INTERFACE_IInArchive(;)
	STDMETHOD(GetStream)(UInt32 index, ISequentialInStream** stream);
};


IMP_IInArchive_Props
IMP_IInArchive_ArcProps_NO_Table

STDMETHODIMP CHandler::GetArchiveProperty(PROPID propID, PROPVARIANT* value)
{
	std::wstringstream ss;
	ss << L"[FormatZzz] GetArchiveProperty: " << propID << std::endl;
	OutputDebugString(ss.str().c_str());

	NCOM::CPropVariant prop;
	switch (propID)
	{
	case kpidPhySize:
		prop = (UInt64)1;
		break;
	}
	prop.Detach(value);
	return S_OK;
}

STDMETHODIMP CHandler::Open(IInStream* stream, const UInt64*, IArchiveOpenCallback* callback)
{
	Close();
	if (!callback)
		return S_FALSE;

	CMyComPtr<IArchiveOpenVolumeCallback> volumeCallback;
	callback->QueryInterface(IID_IArchiveOpenVolumeCallback, (void**)& volumeCallback);
	if (!volumeCallback)
		return S_FALSE;

	UString name;
	{
		NCOM::CPropVariant prop;
		RINOK(volumeCallback->GetProperty(kpidName, &prop));
		if (prop.vt != VT_BSTR)
			return S_FALSE;
		name = prop.bstrVal;
	}

	int dotPos = name.ReverseFind_Dot();
	const UString ext = name.Ptr(dotPos + 1);

	return StringsAreEqualNoCase_Ascii(ext, "zzz") ? S_OK : S_FALSE;
}

STDMETHODIMP CHandler::Close()
{
	return S_OK;
}

STDMETHODIMP CHandler::GetNumberOfItems(UInt32* numItems)
{
	*numItems = _countof(items);
	return S_OK;
}

STDMETHODIMP CHandler::GetProperty(UInt32 index, PROPID propID, PROPVARIANT* value)
{
	std::wstringstream ss;
	ss << L"[FormatZzz] GetProperty: " << index << L", " << propID << std::endl;
	OutputDebugString(ss.str().c_str());
	NCOM::CPropVariant prop;
	switch (propID)
	{
	case kpidPath:
		prop = items[index].name;
		break;
	case kpidSize:
		prop = strlen(items[index].text);
		break;
	case kpidPackSize:
		prop = strlen(items[index].text) * 2;
		break;
	}
	prop.Detach(value);
	return S_OK;
}

STDMETHODIMP CHandler::Extract(const UInt32* indices, UInt32 numItems,
	Int32 testMode, IArchiveExtractCallback* extractCallback)
{
	bool allFilesMode = (numItems == (UInt32)(Int32)-1);
	if (allFilesMode)
		numItems = _countof(items);
	if (numItems == 0)
		return S_OK;
	UInt64 totalSize = 0, currentSize = 0;
	for (size_t i = 0; i < numItems; i++)
	{
		totalSize += strlen(items[allFilesMode ? i : indices[i]].text);
	}
	extractCallback->SetTotal(totalSize);

	CLocalProgress* lps = new CLocalProgress;
	CMyComPtr<ICompressProgressInfo> progress = lps;
	lps->Init(extractCallback, false);

	UINT64 currentItemSize = 0;
	for (UINT i = 0; i < numItems; i++)
	{
		currentItemSize = 0;

		lps->InSize = currentSize;
		lps->OutSize = currentSize;
		RINOK(lps->SetCur());

		CMyComPtr<ISequentialOutStream> realOutStream;
		Int32 askMode = testMode ?
			NExtract::NAskMode::kTest :
			NExtract::NAskMode::kExtract;

		UINT32 index = allFilesMode ? i : indices[i];
		auto& item = items[index];
		currentItemSize = strlen(item.text);
		currentSize += currentItemSize;

		RINOK(extractCallback->GetStream(index, &realOutStream, askMode));

		if (!testMode && !realOutStream)
			continue;

		RINOK(extractCallback->PrepareOperation(askMode));

		RINOK(realOutStream->Write(item.text, (UINT32)strlen(item.text), NULL));
		realOutStream.Release();

		RINOK(extractCallback->SetOperationResult(NExtract::NOperationResult::kOK));
	}
	
	lps->InSize = totalSize;
	lps->OutSize = totalSize;
	return lps->SetCur();
}

STDMETHODIMP CHandler::GetStream(UInt32 index, ISequentialInStream** stream)
{
	std::wstringstream ss;
	ss << L"[FormatZzz] GetStream: " << index << std::endl;
	OutputDebugString(ss.str().c_str());

	*stream = 0;
	CBufInStream* streamSpec = new CBufInStream;
	CMyComPtr<ISequentialInStream> streamTemp = streamSpec;
	streamSpec->Init((const Byte*)items[index].text, strlen(items[index].text));
	*stream = streamTemp.Detach();
	return S_OK;
}

REGISTER_ARC_I_NO_SIG(
	"Zzz", "zzz", 0, 0xAA,
	0,
	0,
	NULL)

}}