/**
 * @file  cliphcat.cpp
 * @brief Clipboard extraction CLI tool
 * @author Takashi Sawanaka
 *
 * SPDX-License-Identifier: MIT
 */

#include <windows.h>
#include <wincodec.h>
#include <Objbase.h>

#if __has_include(<winrt/Windows.ApplicationModel.DataTransfer.h>)
#define CLIPBOARD_HISTORY_SUPPORT 1
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Storage.Streams.h>

#pragma comment(lib, "runtimeobject.lib")
#else
#define CLIPBOARD_HISTORY_SUPPORT 0
#endif

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <regex>
#include <algorithm>

#pragma comment(lib, "windowscodecs.lib")

#if CLIPBOARD_HISTORY_SUPPORT
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::ApplicationModel::DataTransfer;
using namespace winrt::Windows::Storage::Streams;
#endif

// RAII wrapper for COM initialization
class ComInitializer
{
public:
	ComInitializer()
	{
		m_hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	}
	~ComInitializer()
	{
		if (SUCCEEDED(m_hr))
			CoUninitialize();
	}
	bool succeeded() const { return SUCCEEDED(m_hr); }
private:
	HRESULT m_hr;
};

enum ExitCode
{
	EXIT_OK = 0,
	EXIT_CLIPBOARD_EMPTY = 1,
	EXIT_FORMAT_NOT_AVAIL = 2,
	EXIT_INDEX_OUT_OF_RANGE = 3,
	EXIT_HISTORY_NOT_SUPPORTED = 4,
	EXIT_INVALID_ARGS = 5,
};

static void err(const char* msg)
{
	fprintf(stderr, "%s\n", msg);
}

enum FormatType
{
	FMT_AUTO, FMT_TEXT, FMT_HTML, FMT_RTF, FMT_PNG
};

static FormatType ParseFormat(const std::string& s)
{
	if (s == "text")
		return FMT_TEXT;
	if (s == "html")
		return FMT_HTML;
	if (s == "rtf")
		return FMT_RTF;
	if (s == "png")
		return FMT_PNG;
	return FMT_AUTO;
}

static bool WriteOutput(const void* data, size_t size, const std::string& outFile)
{
	if (outFile.empty())
	{
		fwrite(data, 1, size, stdout);
		return true;
	}
	FILE* f = fopen(outFile.c_str(), "wb");
	if (!f)
	{
		err("Failed to open output file.");
		return false;
	}
	fwrite(data, 1, size, f);
	fclose(f);
	return true;
}

static std::string Utf16ToUtf8(const wchar_t* wstr, int wlen = -1)
{
	if (!wstr || wlen == 0)
		return {};
	int len = WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, nullptr, 0, nullptr, nullptr);
	if (len <= 0)
		return {};
	std::string s(len, '\0');
	WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, &s[0], len, nullptr, nullptr);
	if (!s.empty() && s.back() == '\0')
		s.pop_back();
	return s;
}

static std::string Utf16ToUtf8(const std::wstring& ws)
{
	return Utf16ToUtf8(ws.c_str(), (int)ws.size());
}

#if CLIPBOARD_HISTORY_SUPPORT
static std::string HStringToUtf8(const winrt::hstring& hs)
{
	return Utf16ToUtf8(hs.c_str(), (int)hs.size());
}
#endif

template<typename CharT>
static void ReplaceWhitespaceWithSpace(std::basic_string<CharT>& str)
{
	for (auto& c : str)
	{
		if (c == (CharT)'\n' || c == (CharT)'\r' || c == (CharT)'\t')
			c = (CharT)' ';
	}
}

static void TrimTrailingNulls(std::string& str)
{
	while (!str.empty() && str.back() == '\0')
		str.pop_back();
}

static void TrimTrailingNulls(const char* buf, size_t& size)
{
	while (size && buf[size - 1] == '\0')
		size--;
}

static std::vector<uint8_t> ExtractHtmlFragment(const char* buf, size_t bufLen, bool raw)
{
	if (raw)
		return std::vector<uint8_t>(buf, buf + bufLen);
	auto findOffset = [&](const char* key) -> int
		{
			const char* p = strstr(buf, key);
			if (!p)
				return -1;
			return atoi(p + strlen(key));
		};
	int startFrag = findOffset("StartFragment:");
	int endFrag = findOffset("EndFragment:");
	if (startFrag < 0 || endFrag < 0 || startFrag >= endFrag || (size_t)endFrag > bufLen)
		return std::vector<uint8_t>(buf, buf + bufLen);
	return std::vector<uint8_t>(buf + startFrag, buf + endFrag);
}

static std::vector<uint8_t> DibToPng(HGLOBAL hDib)
{
	BITMAPINFO* pBmi = (BITMAPINFO*)GlobalLock(hDib);
	if (!pBmi)
		return {};
	int clrEntries = 0;
	if (pBmi->bmiHeader.biBitCount <= 8)
		clrEntries = pBmi->bmiHeader.biClrUsed ? pBmi->bmiHeader.biClrUsed
		: (1 << pBmi->bmiHeader.biBitCount);
	const BYTE* pBits = (const BYTE*)pBmi + pBmi->bmiHeader.biSize + clrEntries * sizeof(RGBQUAD);
	std::vector<uint8_t> result;
	IWICImagingFactory* pFac = nullptr;
	if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
		IID_IWICImagingFactory, (void**)&pFac)) && pFac)
	{
		HDC hdc = GetDC(nullptr);
		HBITMAP hBmp = CreateDIBitmap(hdc, &pBmi->bmiHeader, CBM_INIT, pBits, pBmi, DIB_RGB_COLORS);
		ReleaseDC(nullptr, hdc);
		IWICBitmap* pBm = nullptr;
		if (hBmp)
		{
			pFac->CreateBitmapFromHBITMAP(hBmp, nullptr, WICBitmapUseAlpha, &pBm);
			DeleteObject(hBmp);
		}
		if (pBm)
		{
			IStream* pSt = nullptr;
			if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &pSt)))
			{
				IWICBitmapEncoder* pEnc = nullptr;
				if (SUCCEEDED(pFac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &pEnc)))
				{
					pEnc->Initialize(pSt, WICBitmapEncoderNoCache);
					IWICBitmapFrameEncode* pFr = nullptr;
					IPropertyBag2* pPr = nullptr;
					if (SUCCEEDED(pEnc->CreateNewFrame(&pFr, &pPr)) && pFr)
					{
						pFr->Initialize(pPr);
						pFr->WriteSource(pBm, nullptr);
						pFr->Commit();
						pFr->Release();
					}
					if (pPr)
						pPr->Release();
					pEnc->Commit();
					pEnc->Release();
				}
				HGLOBAL hm = nullptr;
				if (SUCCEEDED(GetHGlobalFromStream(pSt, &hm)))
				{
					SIZE_T sz = GlobalSize(hm);
					void* p = GlobalLock(hm);
					if (p && sz)
					{
						result.assign((uint8_t*)p, (uint8_t*)p + sz);
						GlobalUnlock(hm);
					}
				}
				pSt->Release();
			}
			pBm->Release();
		}
		pFac->Release();
	}
	GlobalUnlock(hDib);
	return result;
}

static UINT CF_HTML_FORMAT = 0;
static UINT CF_RTF_FORMAT = 0;
static void RegisterFormats()
{
	CF_HTML_FORMAT = RegisterClipboardFormatW(L"HTML Format");
	CF_RTF_FORMAT = RegisterClipboardFormatW(L"Rich Text Format");
}

struct ClipData
{
	std::vector<uint8_t> bytes;
	FormatType fmt = FMT_AUTO;
};

static ClipData GetCurrentClipboard(FormatType requested, bool raw)
{
	ClipData result;
	if (!OpenClipboard(nullptr))
	{
		err("Failed to open clipboard.");
		exit(EXIT_CLIPBOARD_EMPTY);
	}
	std::vector<FormatType> prio = (requested != FMT_AUTO)
		? std::vector<FormatType>{requested}
	: std::vector<FormatType>{ FMT_TEXT, FMT_HTML, FMT_RTF, FMT_PNG };
	for (FormatType fmt : prio)
	{
		if (fmt == FMT_TEXT)
		{
			HANDLE h = GetClipboardData(CF_UNICODETEXT);
			if (!h)
				continue;
			wchar_t* ws = (wchar_t*)GlobalLock(h);
			if (!ws)
				continue;
			if (raw)
			{
				size_t n = wcslen(ws);
				result.bytes.assign((uint8_t*)ws, (uint8_t*)(ws + n));
			}
			else
			{
				auto u = Utf16ToUtf8(ws);
				result.bytes.assign(u.begin(), u.end());
			}
			GlobalUnlock(h);
			result.fmt = FMT_TEXT;
			break;
		}
		else if (fmt == FMT_HTML && CF_HTML_FORMAT)
		{
			HANDLE h = GetClipboardData(CF_HTML_FORMAT);
			if (!h)
				continue;
			char* buf = (char*)GlobalLock(h);
			if (!buf)
				continue;
			size_t sz = GlobalSize(h);
			TrimTrailingNulls(buf, sz);
			result.bytes = ExtractHtmlFragment(buf, sz, raw);
			GlobalUnlock(h);
			result.fmt = FMT_HTML;
			break;
		}
		else if (fmt == FMT_RTF && CF_RTF_FORMAT)
		{
			HANDLE h = GetClipboardData(CF_RTF_FORMAT);
			if (!h)
				continue;
			char* buf = (char*)GlobalLock(h);
			if (!buf)
				continue;
			size_t sz = GlobalSize(h);
			result.bytes.assign((uint8_t*)buf, (uint8_t*)buf + sz);
			GlobalUnlock(h);
			result.fmt = FMT_RTF;
			break;
		}
		else if (fmt == FMT_PNG)
		{
			HANDLE h = GetClipboardData(CF_DIBV5);
			if (!h)
				h = GetClipboardData(CF_DIB);
			if (h)
			{
				if (raw)
				{
					size_t sz = GlobalSize(h);
					void* p = GlobalLock(h);
					if (p)
					{
						result.bytes.assign((uint8_t*)p, (uint8_t*)p + sz);
						GlobalUnlock(h);
						result.fmt = FMT_PNG;
						break;
					}
				}
				else
				{
					result.bytes = DibToPng(h);
					if (!result.bytes.empty())
					{
						result.fmt = FMT_PNG;
						break;
					}
				}
			}
			HBITMAP hBmp = (HBITMAP)GetClipboardData(CF_BITMAP);
			if (hBmp && !raw)
			{
				HDC hdc = GetDC(nullptr);
				BITMAP bm{};
				GetObject(hBmp, sizeof(bm), &bm);
				BITMAPINFOHEADER bih{};
				bih.biSize = sizeof(bih);
				bih.biWidth = bm.bmWidth;
				bih.biHeight = -bm.bmHeight;
				bih.biPlanes = 1;
				bih.biBitCount = 32;
				bih.biCompression = BI_RGB;
				size_t stride = ((bm.bmWidth * 4 + 3) / 4) * 4;
				std::vector<uint8_t> bits(stride * bm.bmHeight);
				GetDIBits(hdc, hBmp, 0, bm.bmHeight, bits.data(), (BITMAPINFO*)&bih, DIB_RGB_COLORS);
				ReleaseDC(nullptr, hdc);
				HGLOBAL hd = GlobalAlloc(GMEM_MOVEABLE, sizeof(bih) + bits.size());
				if (hd)
				{
					void* pd = GlobalLock(hd);
					memcpy(pd, &bih, sizeof(bih));
					memcpy((uint8_t*)pd + sizeof(bih), bits.data(), bits.size());
					GlobalUnlock(hd);
					result.bytes = DibToPng(hd);
					GlobalFree(hd);
					if (!result.bytes.empty())
					{
						result.fmt = FMT_PNG;
						break;
					}
				}
			}
		}
	}
	CloseClipboard();
	return result;
}

#if CLIPBOARD_HISTORY_SUPPORT
static ClipData GetHistoryItemWinRT(int index, FormatType fmt, bool raw)
{
	ClipData result;
	try
	{
		auto historyResult = Clipboard::GetHistoryItemsAsync().get();

		auto items = historyResult.Items();
		UINT32 idx = (UINT32)(index - 1);

		if (idx >= items.Size())
		{
			err("Index out of range.");
			exit(EXIT_INDEX_OUT_OF_RANGE);
		}

		auto item = items.GetAt(idx);
		auto dataView = item.Content();

		std::vector<FormatType> prio = (fmt != FMT_AUTO)
			? std::vector<FormatType>{fmt}
		: std::vector<FormatType>{ FMT_TEXT, FMT_HTML, FMT_RTF };

		for (FormatType f : prio)
		{
			if (f == FMT_TEXT && dataView.Contains(StandardDataFormats::Text()))
			{
				winrt::hstring text = dataView.GetTextAsync().get();
				std::wstring ws(text.c_str());

				if (raw)
				{
					result.bytes.assign((uint8_t*)ws.data(),
						(uint8_t*)(ws.data() + ws.size()));
				}
				else
				{
					auto u = Utf16ToUtf8(ws);
					result.bytes.assign(u.begin(), u.end());
				}
				result.fmt = FMT_TEXT;
				break;
			}
			else if (f == FMT_HTML && dataView.Contains(StandardDataFormats::Html()))
			{
				winrt::hstring html = dataView.GetHtmlFormatAsync().get();
				std::string utf8 = HStringToUtf8(html);
				size_t sz = utf8.size();
				TrimTrailingNulls(utf8.data(), sz);
				result.bytes = ExtractHtmlFragment(utf8.data(), sz, raw);
				result.fmt = FMT_HTML;
				break;
			}
			else if (f == FMT_RTF && dataView.Contains(StandardDataFormats::Rtf()))
			{
				winrt::hstring rtf = dataView.GetRtfAsync().get();
				std::string utf8 = HStringToUtf8(rtf);
				result.bytes.assign(utf8.begin(), utf8.end());
				result.fmt = FMT_RTF;
				break;
			}
		}

	}
	catch (winrt::hresult_error const& ex)
	{
		err("Failed to get clipboard history.");
		fprintf(stderr, "Error: 0x%08X - %s\n", ex.code().value,
			Utf16ToUtf8(ex.message().c_str()).c_str());
		exit(EXIT_CLIPBOARD_EMPTY);
	}
	catch (...)
	{
		err("Unknown error accessing clipboard history.");
		exit(EXIT_CLIPBOARD_EMPTY);
	}

	return result;
}
#endif

static void ListCurrentClipboard(const std::string& outFile)
{
	if (!OpenClipboard(nullptr))
	{
		err("Failed to open clipboard.");
		exit(EXIT_CLIPBOARD_EMPTY);
	}
	std::string out;

	HANDLE h = GetClipboardData(CF_UNICODETEXT);
	if (h)
	{
		wchar_t* ws = (wchar_t*)GlobalLock(h);
		if (ws)
		{
			std::wstring w(ws);
			ReplaceWhitespaceWithSpace(w);
			if (w.size() > 50)
				w = w.substr(0, 50);
			out += "1:text   \"" + Utf16ToUtf8(w) + "\"\n";
			GlobalUnlock(h);
		}
	}

	if (CF_HTML_FORMAT)
	{
		h = GetClipboardData(CF_HTML_FORMAT);
		if (h)
		{
			char* buf = (char*)GlobalLock(h);
			size_t sz = GlobalSize(h);
			TrimTrailingNulls(buf, sz);
			auto frag = ExtractHtmlFragment(buf, sz, false);
			std::string d((char*)frag.data(), std::min(frag.size(), (size_t)50));
			ReplaceWhitespaceWithSpace(d);
			out += "1:html   \"" + d + "\"\n";
			GlobalUnlock(h);
		}
	}

	if (CF_RTF_FORMAT)
	{
		h = GetClipboardData(CF_RTF_FORMAT);
		if (h)
		{
			char* buf = (char*)GlobalLock(h);
			if (buf)
			{
				std::string d(buf, std::min((size_t)50, strlen(buf)));
				ReplaceWhitespaceWithSpace(d);
				out += "1:rtf    \"" + d + "\"\n";
				GlobalUnlock(h);
			}
		}
	}

	if (GetClipboardData(CF_DIB) || GetClipboardData(CF_DIBV5) || GetClipboardData(CF_BITMAP))
		out += "1:png	(image/png)\n";

	CloseClipboard();
	WriteOutput(out.data(), out.size(), outFile);
}

#if CLIPBOARD_HISTORY_SUPPORT
static void ListHistoryWinRT(const std::string& outFile)
{
	try
	{
		auto historyResult = Clipboard::GetHistoryItemsAsync().get();
		auto items = historyResult.Items();

		std::string out;
		for (uint32_t i = 0; i < items.Size(); ++i)
		{
			auto item = items.GetAt(i);
			auto dataView = item.Content();
			int idx = (int)(i + 1);

			if (dataView.Contains(StandardDataFormats::Text()))
			{
				try
				{
					winrt::hstring text = dataView.GetTextAsync().get();
					std::wstring ws(text.c_str());
					ReplaceWhitespaceWithSpace(ws);
					if (ws.size() > 50)
						ws = ws.substr(0, 50);
					out += std::to_string(idx) + ":text   \"" + Utf16ToUtf8(ws) + "\"\n";
				}
				catch (...) {}
			}

			if (dataView.Contains(StandardDataFormats::Html()))
			{
				try
				{
					winrt::hstring html = dataView.GetHtmlFormatAsync().get();
					std::string utf8 = HStringToUtf8(html);
					size_t sz = utf8.size();
					TrimTrailingNulls(utf8.data(), sz);
					auto frag = ExtractHtmlFragment(utf8.data(), sz, false);
					std::string d((char*)frag.data(), std::min(frag.size(), (size_t)50));
					ReplaceWhitespaceWithSpace(d);
					out += std::to_string(idx) + ":html   \"" + d + "\"\n";
				}
				catch (...) {}
			}

			if (dataView.Contains(StandardDataFormats::Rtf()))
			{
				try
				{
					winrt::hstring rtf = dataView.GetRtfAsync().get();
					std::string utf8 = HStringToUtf8(rtf);
					std::string d = utf8.substr(0, std::min(utf8.size(), (size_t)50));
					ReplaceWhitespaceWithSpace(d);
					out += std::to_string(idx) + ":rtf    \"" + d + "\"\n";
				}
				catch (...) {}
			}

			if (dataView.Contains(StandardDataFormats::Bitmap()))
				out += std::to_string(idx) + ":png	(image/png)\n";
		}

		if (out.empty())
			out = "(No clipboard history available)\n";
		WriteOutput(out.data(), out.size(), outFile);

	}
	catch (winrt::hresult_error const& ex)
	{
		fprintf(stderr, "WinRT Error: 0x%08X\n", ex.code().value);
		ListCurrentClipboard(outFile);
	}
	catch (...)
	{
		ListCurrentClipboard(outFile);
	}
}
#endif

static void ListHistory(const std::string& outFile)
{
#if CLIPBOARD_HISTORY_SUPPORT
	try
	{
		ListHistoryWinRT(outFile);
	}
	catch (...)
	{
		ListCurrentClipboard(outFile);
	}
#else
	ListCurrentClipboard(outFile);
#endif
}

static void ClearClipboardHistory()
{
#if CLIPBOARD_HISTORY_SUPPORT
	try
	{
		bool result = Clipboard::ClearHistory();
		if (result)
			fprintf(stdout, "Clipboard history cleared successfully.\n");
		else
			err("Failed to clear clipboard history or history is already empty.");
	}
	catch (winrt::hresult_error const&)
	{
		err("Error clearing clipboard history.");
	}
#endif
	if (OpenClipboard(nullptr))
	{
		EmptyClipboard();
		CloseClipboard();
	}
}

struct ParsedUrl
{
	bool valid = false;
	std::string indexStr;
	FormatType fmt = FMT_AUTO;
};

static ParsedUrl ParseClipUrl(const std::string& url)
{
	ParsedUrl r;
	std::regex re(R"((?:clip|clipboard)://([^?]+)(?:\?format=(\w+))?)", std::regex::icase);
	std::smatch m;
	if (!std::regex_match(url, m, re))
		return r;
	r.valid = true;
	r.indexStr = m[1].str();
	if (m[2].matched)
		r.fmt = ParseFormat(m[2].str());
	return r;
}

static int ResolveIndex(const std::string& s)
{
	if (s == "latest" || s == "1")
		return 1;
	if (s == "previous" || s == "2")
		return 2;
	try
	{
		return std::stoi(s);
	}
	catch (...) {}
	return -1;
}

static ClipData GetClipboardByIndex(int index, FormatType fmt, bool raw)
{
	if (index == 1)
		return GetCurrentClipboard(fmt, raw);
#if CLIPBOARD_HISTORY_SUPPORT
	return GetHistoryItemWinRT(index, fmt, raw);
#else
	err("Clipboard history not supported in this build.");
	exit(EXIT_HISTORY_NOT_SUPPORTED);
#endif
}

struct Options
{
	std::string outFile;
	FormatType fmt = FMT_AUTO;
	bool raw = false;
	bool listMode = false;
	bool clearMode = false;
	std::string indexStr = "1";
};

static int ParseArguments(int argc, char* argv[], Options& opts)
{
	bool indexSet = false;

	for (int i = 1; i < argc; i++)
	{
		std::string arg = argv[i];
		if (arg == "-o" || arg == "--output")
		{
			if (i + 1 >= argc)
			{
				err("Missing value for -o.");
				return EXIT_INVALID_ARGS;
			}
			opts.outFile = argv[++i];
		}
		else if (arg == "-f" || arg == "--format")
		{
			if (i + 1 >= argc)
			{
				err("Missing value for -f.");
				return EXIT_INVALID_ARGS;
			}
			opts.fmt = ParseFormat(argv[++i]);
			if (opts.fmt == FMT_AUTO)
			{
				err("Invalid format. Use: text, html, rtf, png");
				return EXIT_INVALID_ARGS;
			}
		}
		else if (arg == "--raw")
		{
			opts.raw = true;
		}
		else if (arg == "-l" || arg == "--list")
		{
			opts.listMode = true;
		}
		else if (arg == "-C" || arg == "--clear")
		{
			opts.clearMode = true;
		}
		else if (arg == "-h" || arg == "--help")
		{
			fprintf(stdout,
				"cliphcat [options] [index|alias|url]\n\n"
				"Arguments:\n"
				"  1, latest	Current clipboard (default)\n"
				"  2, previous  Previous clipboard history item\n"
				"  N			History item N (1=latest)\n"
				"  clip://N	 URL scheme (clipboard://, clip://)\n\n"
				"Options:\n"
				"  -o, --output <file>   Output to file instead of stdout\n"
				"  -f, --format <type>   text | html | rtf | png\n"
				"  --raw				 Output raw clipboard data\n"
				"  -l, --list			List clipboard history\n"
				"  -C, --clear			Clear clipboard history\n"
				"  -h, --help			Show this help\n");
			return EXIT_OK;
		}
		else if (arg[0] != '-')
		{
			if (!indexSet)
			{
				opts.indexStr = arg;
				indexSet = true;
			}
			else
			{
				err("Unexpected extra argument.");
				return EXIT_INVALID_ARGS;
			}
		}
		else
		{
			err(("Unknown option: " + arg).c_str());
			return EXIT_INVALID_ARGS;
		}
	}

	return -1; // Continue processing
}

int main(int argc, char* argv[])
{
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCP(CP_UTF8);

	ComInitializer comInit;
	RegisterFormats();

	Options opts;
	int parseResult = ParseArguments(argc, argv, opts);
	if (parseResult >= 0)
		return parseResult;

	if (opts.listMode)
	{
		ListHistory(opts.outFile);
		return EXIT_OK;
	}

	if (opts.clearMode)
	{
		ClearClipboardHistory();
		return EXIT_OK;
	}

	int index = 1;
	if (opts.indexStr.find("://") != std::string::npos)
	{
		ParsedUrl p = ParseClipUrl(opts.indexStr);
		if (!p.valid)
		{
			err("Invalid URL format.");
			return EXIT_INVALID_ARGS;
		}
		index = ResolveIndex(p.indexStr);
		if (opts.fmt == FMT_AUTO && p.fmt != FMT_AUTO)
			opts.fmt = p.fmt;
	}
	else
	{
		index = ResolveIndex(opts.indexStr);
	}

	if (index <= 0)
	{
		err("Index out of range.");
		return EXIT_INDEX_OUT_OF_RANGE;
	}

	ClipData data = GetClipboardByIndex(index, opts.fmt, opts.raw);

	if (data.bytes.empty())
	{
		if (opts.fmt != FMT_AUTO && data.fmt != opts.fmt)
		{
			err("Requested format not available.");
			return EXIT_FORMAT_NOT_AVAIL;
		}
		err("Clipboard is empty.");
		return EXIT_CLIPBOARD_EMPTY;
	}

	if (opts.fmt != FMT_AUTO && data.fmt != FMT_AUTO && data.fmt != opts.fmt)
	{
		err("Requested format not available.");
		return EXIT_FORMAT_NOT_AVAIL;
	}

	bool ok = WriteOutput(data.bytes.data(), data.bytes.size(), opts.outFile);
	return ok ? EXIT_OK : EXIT_INVALID_ARGS;
}

