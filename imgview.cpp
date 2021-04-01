// TODO gifs, rotate, crop

#include <windows.h> 
#include <windowsx.h> 
#include <commctrl.h> 
#include <shellscalingapi.h>
#include <stdint.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <exdisp.h>
#include <GdiPlus.h>

using namespace Gdiplus;
using namespace Gdiplus::DllExports;

/////////////////////////////////////////////////////////////////

#ifndef _WIN64 
#ifdef __cplusplus 
extern "C"
#endif
__declspec(naked) void _ftol2_sse() {
	__asm {
		fistp	dword ptr [esp - 4]
		mov	eax,[esp - 4]
		ret
	}
}
#endif

extern "C" int _fltused = 0;
typedef HRESULT (*GetDpiForMonitorType)(HMONITOR hmonitor, MONITOR_DPI_TYPE dpiType, UINT *dpiX, UINT *dpiY);
typedef BOOL (*SetProcessDpiAwarenessContextType)(DPI_AWARENESS_CONTEXT value);
HMODULE shcore, instance;
HANDLE processHeap;

HWND windowFrame, windowViewport, windowStatusBar;
HBRUSH brushDarkBackground, brushLightBackground;
bool usingDarkBackground, hideStatusBar;
HCURSOR cursorPan;

UINT imageAnchorX, imageAnchorY;
UINT imageWidth, imageHeight;
GpBitmap *imageObject;
bool imageError, imageChanged;

DWORD threadMain;

struct FolderEntry { wchar_t path[MAX_PATH]; };
FolderEntry *filesInFolder;
int indexInFolder, itemsInFolder;
bool filesInFolderReady;
bool clipboardMode;

float panX = 0, panY = 0, zoom = 1.0f;
bool zoomFit = false;
POINT lastPanPoint;
bool panningViewport;
int lastMouseX, lastMouseY;

/////////////////////////////////////////////////////////////////

#pragma function(memset)
void *memset(void *s, int c, size_t n) {
	char *s2 = (char *) s;
	while (n--) *s2++ = c;
	return s;
}

#pragma function(memcpy)
void *memcpy(void *d, const void *s, size_t n) {
	char *s2 = (char *) s, *d2 = (char *) d;
	while (n--) *d2++ = *s2++;
	return d;
}

#pragma function(strlen)
size_t strlen(const char *s) {
	size_t c = 0;
	for (; *s; s++) c++;
	return c;
}

bool StringEndsWith(const wchar_t *p, const wchar_t *p2) {
	size_t c = 0;
	for (; *p; p++) c++;
	size_t c2 = 0;
	for (; *p2; p2++) c2++;
	if (c < c2) return false;
	
	for (int i = 0; i < c2; i++) {
		if (p[-1 - i] != p2[-1 - i]) {
			return false;
		}
	}

	return true;
}

void StringCopy(wchar_t *d, const wchar_t *s) {
	while (true) {
		wchar_t c = *s++;
		*d++ = c;
		if (!c) break;
	}
}

void StringAppend(wchar_t *d, const wchar_t *s) {
	while (*d) d++;
	StringCopy(d, s);
}

void StringAppendInteger(wchar_t *d, int integer) {
	while (*d) d++;
	wchar_t buffer[16];
	int position = 0;
	
	if (integer < 0) {
		integer = -integer;
		*d++ = '-';
	}
	
	if (!integer) {
		buffer[position++] = '0';
	}
	
	while (integer) {
		buffer[position++] = (integer % 10) + '0';
		integer /= 10;
	}
	
	while (position) {
		*d++ = buffer[--position];
	}
	
	*d++ = 0;
}

void StringAppendHexInteger(wchar_t *d, uint32_t integer) {
	while (*d) d++;
	wchar_t *hexChars = L"0123456789ABCDEF";
	*d++ = hexChars[(integer >> 28) & 0xF];
	*d++ = hexChars[(integer >> 24) & 0xF];
	*d++ = hexChars[(integer >> 20) & 0xF];
	*d++ = hexChars[(integer >> 16) & 0xF];
	*d++ = hexChars[(integer >> 12) & 0xF];
	*d++ = hexChars[(integer >>  8) & 0xF];
	*d++ = hexChars[(integer >>  4) & 0xF];
	*d++ = hexChars[(integer >>  0) & 0xF];
	*d++ = 0;
}

bool StringEqual(const wchar_t *a, const wchar_t *b) {
	while (true) {
		if (*a != *b) return false;
		if (*a == 0) return true;
		a++, b++;
	}
}

#pragma function(memmove)
void *memmove(void *d, const void *s, size_t n) {
	char *s2 = (char *) s, *d2 = (char *) d;
	if (d2 < s2) { while (n--) *d2++ = *s2++; return d; }
	s2 += n, d2 += n;
	while (n--) *(--d2) = *(--s2);
	return d;
}

void *malloc(size_t size) {
	return HeapAlloc(processHeap, 0, size);
}

void free(void *pointer) {
	HeapFree(processHeap, 0, pointer);
}

#define Assert(x) do { if (!(x)) AssertionFailed(); } while (0)
void AssertionFailed() { MessageBox(windowFrame, "An internal error has occurred.", 0, MB_OK); ExitProcess(1); }

float LinearMap(float value, float inFrom, float inTo, float outFrom, float outTo) {
	float inRange = inTo - inFrom, outRange = outTo - outFrom;
	float normalisedValue = (value - inFrom) / inRange;
	return normalisedValue * outRange + outFrom;
}

/////////////////////////////////////////////////////////////////

void UpdateStatusBar() {
	if (!imageObject) return;

	RECT bounds;
	GetClientRect(windowViewport, &bounds);
		
	POINT point;
	GetCursorPos(&point);
	ScreenToClient(windowViewport, &point);
	
	int x = (int) LinearMap(point.x + 0.5f, 0, bounds.right, panX, panX + bounds.right / zoom);
	int y = (int) LinearMap(point.y + 0.5f, 0, bounds.bottom, panY, panY + bounds.bottom / zoom);
	lastMouseX = x, lastMouseY = y;
	
	uint32_t color = 0;
	
	if (x >= 0 && y >= 0 && x < imageWidth && y < imageHeight) {
		GdipBitmapGetPixel(imageObject, x, y, (ARGB *) &color);
	}
	
	wchar_t buffer[256];
	
	StringCopy(buffer, L"Image size: ");
	StringAppendInteger(buffer, imageWidth);
	StringAppend(buffer, L"x");
	StringAppendInteger(buffer, imageHeight);
	SendMessage(windowStatusBar, SB_SETTEXTW, 0, (LPARAM) buffer);
	
	StringCopy(buffer, L"Mouse coordinates: ");
	StringAppendInteger(buffer, x);
	StringAppend(buffer, L",");
	StringAppendInteger(buffer, y);
	SendMessage(windowStatusBar, SB_SETTEXTW, 1, (LPARAM) buffer);
	
	StringCopy(buffer, L"Color under mouse: #");
	StringAppendHexInteger(buffer, color);
	SendMessage(windowStatusBar, SB_SETTEXTW, 2, (LPARAM) buffer);
	
	StringCopy(buffer, L"Anchor offset: ");
	StringAppendInteger(buffer, x - imageAnchorX);
	StringAppend(buffer, L",");
	StringAppendInteger(buffer, y - imageAnchorY);
	SendMessage(windowStatusBar, SB_SETTEXTW, 3, (LPARAM) buffer);
}

void UpdateViewport(bool center = false) {
	RECT bounds;
	GetClientRect(windowViewport, &bounds);
	
	float minimumZoomX = 1, minimumZoomY = 1;
	if (imageWidth > bounds.right) minimumZoomX = (float) bounds.right / imageWidth;
	if (imageHeight > bounds.bottom) minimumZoomY = (float) bounds.bottom / imageHeight;
	float minimumZoom = minimumZoomX < minimumZoomY ? minimumZoomX : minimumZoomY;
	
	if (zoom < minimumZoom || (imageChanged && (~GetKeyState(VK_SHIFT) & 0x8000)) || zoomFit) {
		zoom = minimumZoom;
		zoomFit = true;
	}
	
	imageChanged = false;
	
	if (panX < 0) panX = 0;
	if (panX > imageWidth - bounds.right / zoom) panX = imageWidth - bounds.right / zoom;
	if (panY < 0) panY = 0;
	if (panY > imageHeight - bounds.bottom / zoom) panY = imageHeight - bounds.bottom / zoom;
	
	if (imageWidth * zoom <= bounds.right || center) {
		panX = imageWidth / 2 - bounds.right / zoom / 2;
	}
	
	if (imageHeight * zoom <= bounds.bottom || center) {
		panY = imageHeight / 2 - bounds.bottom / zoom / 2;
	}
	
	RedrawWindow(windowViewport, NULL, NULL, RDW_INVALIDATE | RDW_ERASE);
	UpdateStatusBar();
}

LRESULT CALLBACK ViewportProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
	if (message == WM_PAINT) {
		PAINTSTRUCT paint;
		BeginPaint(window, &paint);
		RECT bounds;
		GetClientRect(window, &bounds);
		
		int x = (int) (0.5f + LinearMap(0, panX, panX + bounds.right / zoom, 0, bounds.right));
		int y = (int) (0.5f + LinearMap(0, panY, panY + bounds.bottom / zoom, 0, bounds.bottom));	

		GpGraphics *graphics;
		GdipCreateFromHDC(paint.hdc, &graphics);
		GdipSetInterpolationMode(graphics, zoom < 1 ? InterpolationModeHighQualityBilinear : InterpolationModeNearestNeighbor);
		GdipDrawImageRect(graphics, imageObject, x, y, imageWidth * zoom, imageHeight * zoom);
		GdipDeleteGraphics(graphics);

		EndPaint(window, &paint);
	} else if (message == WM_LBUTTONDOWN) {
		panningViewport = true;
		GetCursorPos(&lastPanPoint);
		SetCursor(cursorPan);
		zoomFit = false;
	} else if (message == WM_LBUTTONUP) {
		ReleaseCapture();
		panningViewport = false;
	} else if (message == WM_SIZE) {
		static int previousWidth = 0, previousHeight = 0;
		RECT bounds;
		GetClientRect(window, &bounds);
		panX -= (bounds.right - previousWidth) / 2 / zoom;
		panY -= (bounds.bottom - previousHeight) / 2 / zoom;
		previousWidth = bounds.right, previousHeight = bounds.bottom;
		UpdateViewport();
	} else if (message == WM_MOUSEMOVE && panningViewport) {
		SetCapture(window);
		SetCursor(cursorPan);
		POINT point;
		GetCursorPos(&point);
		panX -= (point.x - lastPanPoint.x) / zoom;
		panY -= (point.y - lastPanPoint.y) / zoom;
		lastPanPoint = point;
		UpdateViewport();
	} else if (message == WM_MOUSEMOVE) {
		UpdateStatusBar();
	} else {
		return DefWindowProc(window, message, wParam, lParam);
	}
	
	return 0;
}

void LoadClipboard() {
	if (!IsClipboardFormatAvailable(CF_BITMAP) && !IsClipboardFormatAvailable(CF_DIB) && !IsClipboardFormatAvailable(CF_DIBV5)) return;	
	OpenClipboard(windowFrame);
	HANDLE handle = GetClipboardData(CF_DIB);
	if (!handle) handle = GetClipboardData(CF_DIBV5);
	
	if (handle) {
		void *data = GlobalLock(handle);
		size_t size = GlobalSize(handle) + 14;
		uint8_t *header = (uint8_t *) malloc(size);
		
		uint16_t signature = 0x4D42, zero = 0;
		uint32_t offset = 14 + *((uint32_t *) data);
		memcpy(header + 0, &signature, 2);
		memcpy(header + 2, &size, 4);
		memcpy(header + 6, &zero, 2);
		memcpy(header + 8, &zero, 2);
		memcpy(header + 10, &offset, 4);
		memcpy(header + 14, data, size - 14);
		
		GlobalUnlock(handle);
		CloseClipboard();
		
		IStream *stream = SHCreateMemStream(header, size);
		
		if (stream) {
			if (imageObject) {
				GdipDisposeImage(imageObject);
				imageObject = NULL;
			}
			
			if (Ok != GdipCreateBitmapFromStream(stream, &imageObject)) {
				imageError = true;
				return;
			}
			
			imageError = false;
			GdipGetImageWidth(imageObject, &imageWidth);
			GdipGetImageHeight(imageObject, &imageHeight);
			
			stream->Release();
			
			imageChanged = true;
		}
		
		free(header);
	} else {
		CloseClipboard();
	}
}

void LoadImage(wchar_t *fileName) {
	if (imageObject) {
		GdipDisposeImage(imageObject);
		imageObject = NULL;
	}
	
	if (!StringEndsWith(fileName, L".bmp")
			&& !StringEndsWith(fileName, L".BMP")
			&& !StringEndsWith(fileName, L".gif")
			&& !StringEndsWith(fileName, L".GIF")
			&& !StringEndsWith(fileName, L".jpeg")
			&& !StringEndsWith(fileName, L".JPEG")
			&& !StringEndsWith(fileName, L".jpg")
			&& !StringEndsWith(fileName, L".JPG")
			&& !StringEndsWith(fileName, L".png")
			&& !StringEndsWith(fileName, L".PNG")
			&& !StringEndsWith(fileName, L".tif")
			&& !StringEndsWith(fileName, L".TIF")
			&& !StringEndsWith(fileName, L".tiff")
			&& !StringEndsWith(fileName, L".TIFF")
			&& !StringEndsWith(fileName, L".wmf")
			&& !StringEndsWith(fileName, L".WMF")
			&& !StringEndsWith(fileName, L".emf")
			&& !StringEndsWith(fileName, L".EMF")) {
		imageError = true;
		return;	
	}
	
	IStream *stream = NULL;
	
	{
		bool success = false;
		
		HANDLE handle = INVALID_HANDLE_VALUE;
		BYTE *buffer = NULL;
		DWORD fileSize = 0, fileSizeHigh = 0, bytesRead = 0;
		
		handle = CreateFileW(fileName, GENERIC_READ, FILE_SHARE_READ, 0, 
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
		if (handle == INVALID_HANDLE_VALUE) goto error;
		
		fileSize = GetFileSize(handle, &fileSizeHigh);
		if (fileSizeHigh || fileSize == 0xFFFFFFFF) goto error;
		
		buffer = (BYTE *) malloc(fileSize);
		if (!buffer) goto error;
		
		if (!ReadFile(handle, buffer, fileSize, &bytesRead, NULL)) goto error;
		if (bytesRead != fileSize) goto error;  
		
		stream = SHCreateMemStream(buffer, fileSize);
		if (!stream) goto error;
		
		success = true;
		error:;
		if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
		if (stream && !success) stream->Release();
		if (buffer) free(buffer);
		
		if (!success) {
			imageError = true;
			return;
		}
	}
	
	if (Ok != GdipCreateBitmapFromStream(stream, &imageObject)) {
		stream->Release();
		imageError = true;
		return;
	}
	
	stream->Release();
	imageError = false;
	imageChanged = true;
	GdipGetImageWidth(imageObject, &imageWidth);
	GdipGetImageHeight(imageObject, &imageHeight);
	
	GpBitmap *result = NULL;
	
	if (Ok == GdipCloneBitmapArea(0, 0, imageWidth, imageHeight, PixelFormat32bppARGB, 
			imageObject, &result)) {
		GdipDisposeImage(imageObject);
		imageObject = result;
	}
}

LRESULT CALLBACK FrameProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
	if (message == WM_CLOSE) {
		ExitProcess(0);
	} else if (message == WM_DPICHANGED) {
		RECT *newBounds = (RECT *) lParam;
		MoveWindow(window, newBounds->left, newBounds->top, newBounds->right - newBounds->left, newBounds->bottom - newBounds->top, TRUE);
	} else if (message == WM_SIZE) {
		RECT bounds, statusBarBounds;
		GetClientRect(window, &bounds);
		
		if (hideStatusBar) {
			statusBarBounds.top = statusBarBounds.bottom = 0;
			ShowWindow(windowStatusBar, SW_HIDE);
		} else {
			SendMessage(windowStatusBar, WM_SIZE, 0, 0);
			GetWindowRect(windowStatusBar, &statusBarBounds);
			ShowWindow(windowStatusBar, SW_SHOW);
		}
		
		MoveWindow(windowViewport, 0, 0, bounds.right, bounds.bottom - (statusBarBounds.bottom - statusBarBounds.top), TRUE);
	} else if (message == WM_KEYDOWN && wParam == VK_ESCAPE) {
		ExitProcess(0);
	} else if (message == WM_MOUSEWHEEL) {
		zoomFit = false;
		int divisions = GET_WHEEL_DELTA_WPARAM(wParam) / 120;
		float factor = 1;
		float perDivision = (GetKeyState(VK_CONTROL) & 0x8000) ? 2.0f 
			: (GetKeyState(VK_MENU) & 0x8000) ? 1.01f : 1.2f;
		while (divisions > 0) factor *= perDivision, divisions--;
		while (divisions < 0) factor /= perDivision, divisions++;
		if (zoom * factor > 64) factor = 64 / zoom;
		
		POINT point;
		GetCursorPos(&point);
		ScreenToClient(windowViewport, &point);
		
		zoom *= factor;
		panX -= point.x / zoom * (1 - factor);
		panY -= point.y / zoom * (1 - factor);
		
		UpdateViewport();
	} else if (message == WM_CONTEXTMENU) {
		if ((HWND) wParam == windowViewport || (HWND) wParam == window) {
			int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
			
			if (x == -1 && y == -1) {
				RECT bounds;
				GetWindowRect(windowViewport, &bounds);
				x = bounds.left, y = bounds.top;
			}
			
			static HMENU menuRightClick = NULL;
			if (menuRightClick) DestroyMenu(menuRightClick);
			menuRightClick = CreatePopupMenu();
			
			if (filesInFolderReady) {
				InsertMenu(menuRightClick, -1, MF_BYPOSITION | MF_STRING, 1, "&Show in folder\tCtrl+O");
				InsertMenu(menuRightClick, -1, MF_BYPOSITION | MF_STRING, 2, "Cop&y to clipboard\tCtrl+C");
				InsertMenu(menuRightClick, -1, MF_BYPOSITION | MF_STRING, 3, "Copy as pa&th\tCtrl+Shift+C");
				InsertMenu(menuRightClick, -1, MF_BYPOSITION | MF_SEPARATOR, 0, 0);
				InsertMenu(menuRightClick, -1, MF_BYPOSITION | MF_STRING | MF_GRAYED, 0, "Hold shift to keep pan and zoom");
				InsertMenu(menuRightClick, -1, MF_BYPOSITION | MF_STRING, 4, "&Previous\tLeft");
				InsertMenu(menuRightClick, -1, MF_BYPOSITION | MF_STRING, 5, "&Next\tRight");
			}
			
			InsertMenu(menuRightClick, -1, MF_BYPOSITION | MF_STRING, 6, "Re&fresh\tF5");
			InsertMenu(menuRightClick, -1, MF_BYPOSITION | MF_SEPARATOR, 0, 0);
#if 0
			InsertMenu(menuRightClick, -1, MF_BYPOSITION | MF_STRING, 7, "Rotate &anticlockwise\tCtrl+Left");
			InsertMenu(menuRightClick, -1, MF_BYPOSITION | MF_STRING, 8, "Rotate &clockwise\tCtrl+Right");
			InsertMenu(menuRightClick, -1, MF_BYPOSITION | MF_STRING, 9, "C&rop\tCtrl+R");
			InsertMenu(menuRightClick, -1, MF_BYPOSITION | MF_SEPARATOR, 0, 0);
#endif
			InsertMenu(menuRightClick, -1, MF_BYPOSITION | MF_STRING, 10, "S&et anchor\tCtrl+M");
			InsertMenu(menuRightClick, -1, MF_BYPOSITION | MF_SEPARATOR, 0, 0);
			InsertMenu(menuRightClick, -1, MF_BYPOSITION | MF_STRING, 11, "Toggle &dark\tCtrl+D");
			InsertMenu(menuRightClick, -1, MF_BYPOSITION | MF_STRING, 15, "Toggle status &bar\tCtrl+B");
			InsertMenu(menuRightClick, -1, MF_BYPOSITION | MF_SEPARATOR, 0, 0);
			InsertMenu(menuRightClick, -1, MF_BYPOSITION | MF_STRING, 12, "Fit to &window\tCtrl+0");
			InsertMenu(menuRightClick, -1, MF_BYPOSITION | MF_STRING, 13, "Actual si&ze\tCtrl+1");
			InsertMenu(menuRightClick, -1, MF_BYPOSITION | MF_SEPARATOR, 0, 0);
			InsertMenu(menuRightClick, -1, MF_BYPOSITION | MF_STRING, 14, "Count c&olors\tCtrl+L");
			
			TrackPopupMenu(menuRightClick, 0, x, y, 0, window, 0);
		}
	} else if (message == WM_COMMAND) {
		if (lParam == 0 && HIWORD(wParam) == 0) {
			if (LOWORD(wParam) == 1 && filesInFolderReady) {
				PROCESS_INFORMATION info;
				STARTUPINFOW startup = { sizeof(STARTUPINFOW) };
				wchar_t buffer[MAX_PATH * 2];
				StringCopy(buffer, L"explorer.exe /select,\"");
				StringAppend(buffer, filesInFolder[indexInFolder].path);
				StringAppend(buffer, L"\"");
				
				if (CreateProcessW(0, buffer, 0, 0, 0, 0, 0, 0, &startup, &info)) {
					CloseHandle(info.hProcess);
					CloseHandle(info.hThread);
				}
			} else if (LOWORD(wParam) == 2) {
				HBITMAP copyBitmap = CreateBitmap(imageWidth, imageHeight, 1, 32, NULL);
				HDC screenDC = GetDC(NULL);
				HDC destinationDC = CreateCompatibleDC(screenDC);
				HBITMAP oldDestinationBitmap = (HBITMAP) SelectObject(destinationDC, copyBitmap);
				
				GpGraphics *graphics;
				GdipCreateFromHDC(destinationDC, &graphics);
				GdipDrawImageRect(graphics, imageObject, 0, 0, imageWidth, imageHeight);
				GdipDeleteGraphics(graphics);
				
				SelectObject(destinationDC, oldDestinationBitmap);
				ReleaseDC(NULL, screenDC);
				DeleteDC(destinationDC);
				
				OpenClipboard(window);
				EmptyClipboard();
				SetClipboardData(CF_BITMAP, copyBitmap);
				CloseClipboard();
			} else if (LOWORD(wParam) == 3 && filesInFolderReady) {
				OpenClipboard(window);
				EmptyClipboard();
				HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, MAX_PATH * sizeof(wchar_t));
				void *copy = GlobalLock(memory);
				StringCopy((wchar_t *) copy, filesInFolder[indexInFolder].path);
				GlobalUnlock(copy);
				SetClipboardData(CF_UNICODETEXT, memory);
				CloseClipboard();
			} else if (LOWORD(wParam) == 4 && filesInFolderReady) {
				int start = indexInFolder;
				
				do {
					if (indexInFolder) indexInFolder--;
					else indexInFolder = itemsInFolder - 1;
					LoadImage(filesInFolder[indexInFolder].path);
				} while (imageError && indexInFolder != start);
				
				SetWindowTextW(windowFrame, filesInFolder[indexInFolder].path);
				UpdateViewport();
			} else if (LOWORD(wParam) == 5 && filesInFolderReady) {
				int start = indexInFolder;
				
				do {
					if (indexInFolder != itemsInFolder - 1) indexInFolder++;
					else indexInFolder = 0;
					LoadImage(filesInFolder[indexInFolder].path);
				} while (imageError && indexInFolder != start);
				
				SetWindowTextW(windowFrame, filesInFolder[indexInFolder].path);
				UpdateViewport();
			} else if (LOWORD(wParam) == 6) {
				if (filesInFolderReady) {
					LoadImage(filesInFolder[indexInFolder].path);
				} else if (clipboardMode) {
					LoadClipboard();
				}
				
				UpdateViewport();
			} else if (LOWORD(wParam) == 10) {
				imageAnchorX = lastMouseX, imageAnchorY = lastMouseY;
			} else if (LOWORD(wParam) == 11) {
				usingDarkBackground = !usingDarkBackground;
				SetClassLongPtr(windowViewport, GCLP_HBRBACKGROUND, (LONG_PTR) (usingDarkBackground ? brushDarkBackground : brushLightBackground));
				RedrawWindow(windowViewport, NULL, NULL, RDW_INVALIDATE | RDW_ERASE);
			} else if (LOWORD(wParam) == 12) {
				zoomFit = true;
				UpdateViewport();
			} else if (LOWORD(wParam) == 13) {
				zoom = 1;
				zoomFit = false;
				UpdateViewport(true);
			} else if (LOWORD(wParam) == 14 && !imageError && imageObject) {
				int count = 0;
				uint8_t *memory = (uint8_t *) HeapAlloc(processHeap, HEAP_ZERO_MEMORY, 0x10000 * (sizeof(uint16_t *) + sizeof(uint16_t)));
				uint16_t **bins = (uint16_t **) memory;
				uint16_t *usage = (uint16_t *) (memory + 0x10000 * sizeof(uint16_t *));
				
				for (int i = 0; i < imageWidth; i++) {
					for (int j = 0; j < imageHeight; j++) {
						uint32_t color;
						GdipBitmapGetPixel(imageObject, i, j, (ARGB *) &color);
						
						if (!(color & 0xFF000000)) {
							color = 0; // All completely transparent pixels are the same.
						}
						
						uint16_t slot = color & 0xFFFF;
						
						if (usage[slot] == 0) {
							usage[slot] = 1;
							bins[slot] = (uint16_t *) HeapAlloc(processHeap, 0, 4 * sizeof(uint16_t));
							bins[slot][0] = color >> 16;
							count++;
							goto nextPixel;
						}
						
						uint16_t *bin = bins[slot];
						
						for (int i = 0; i < usage[slot]; i++) {
							if (bin[i] == (color >> 16)) {
								goto nextPixel;
							}
						}
						
						int max = HeapSize(processHeap, 0, bin) / sizeof(uint16_t);
						
						if (max == usage[slot]) {
							bins[slot] = (uint16_t *) HeapReAlloc(processHeap, 0, bin, max * 2 * sizeof(uint16_t));
							bin = bins[slot];
						}
						
						bin[usage[slot]] = color >> 16;
						usage[slot]++;
						count++;
						
						nextPixel:;
					}
				}
				
				for (int i = 0; i < 0x10000; i++) {
					if (bins[i]) {
						HeapFree(processHeap, 0, bins[i]);
					}
				}
				
				HeapFree(processHeap, 0, memory);
				
				wchar_t buffer[64];
				buffer[0] = 0;
				StringAppendInteger(buffer, count);
				StringAppend(buffer, L" unique colors in image");
				MessageBoxW(windowFrame, buffer, L"imgview", MB_OK);
			} else if (LOWORD(wParam) == 15) {
				hideStatusBar = !hideStatusBar;
				SendMessage(window, WM_SIZE, 0, 0);
			}
		}
	} else {
		return DefWindowProc(window, message, wParam, lParam);
	}

	return 0;
}

DWORD WINAPI GetFolderContents(void *_filePath) {
	wchar_t *filePath = (wchar_t *) _filePath;
	wchar_t pathBuffer[MAX_PATH + 4];
	
	IShellWindows *shellWindows = NULL;
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	if (S_OK != CoCreateInstance(CLSID_ShellWindows, NULL, CLSCTX_ALL, IID_IShellWindows, (void **) &shellWindows)) return 0;
	
	IDispatch *dispatch = NULL;
	VARIANT v = {};
	V_VT(&v) = VT_I4;
	
	for (V_I4(&v) = 0; S_OK == shellWindows->Item(v, &dispatch); V_I4(&v)++) {
		bool success = false;
		
		IFolderView *folderView = NULL;	
		IWebBrowserApp *webBrowserApp = NULL;
		IServiceProvider *serviceProvider = NULL;
		IShellBrowser *shellBrowser = NULL;
		IShellView *shellView = NULL;
		IPersistFolder2 *persistFolder = NULL;
		ITEMIDLIST *folderPIDL = NULL;
		ITEMIDLIST *itemPIDL = NULL;
		PIDLIST_ABSOLUTE fullPIDL = NULL;
		
		int itemCount = 0, focusedItem = 0;
		pathBuffer[0] = 0;
		
		if (S_OK != dispatch->QueryInterface(IID_IWebBrowserApp, (void **) &webBrowserApp)) goto error;
		if (S_OK != webBrowserApp->QueryInterface(IID_IServiceProvider, (void **) &serviceProvider)) goto error;
		if (S_OK != serviceProvider->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, (void **) &shellBrowser)) goto error;
		if (S_OK != shellBrowser->QueryActiveShellView(&shellView)) goto error;
		if (S_OK != shellView->QueryInterface(IID_IFolderView, (void **) &folderView)) goto error;
		if (S_OK != folderView->GetFolder(IID_IPersistFolder2, (void **) &persistFolder)) goto error;
		if (S_OK != persistFolder->GetCurFolder(&folderPIDL)) goto error;
		if (S_OK != folderView->GetFocusedItem(&focusedItem)) goto error;
		if (S_OK != folderView->Item(focusedItem, &itemPIDL)) goto error;
		fullPIDL = ILCombine(folderPIDL, itemPIDL);
		if (!SHGetPathFromIDListW(fullPIDL, pathBuffer)) goto error;
		if (!StringEqual(filePath, pathBuffer)) goto error;
		if (S_OK != folderView->ItemCount(SVGIO_ALLVIEW, &itemCount)) goto error;
		if (!(filesInFolder = (FolderEntry *) malloc(itemCount * sizeof(FolderEntry)))) goto error;
		
		for (int i = 0; i < itemCount; i++) {
			filesInFolder[i].path[0] = 0;
			ITEMIDLIST *itemPIDL = NULL;
			if (S_OK != folderView->Item(i, &itemPIDL)) continue;
			PIDLIST_ABSOLUTE fullPIDL = ILCombine(folderPIDL, itemPIDL);
			SHGetPathFromIDListW(fullPIDL, filesInFolder[i].path);
			CoTaskMemFree(fullPIDL);
			CoTaskMemFree(itemPIDL);
		}
		
		itemsInFolder = itemCount;
		indexInFolder = focusedItem;
		PostThreadMessage(threadMain, WM_APP + 1, 0, 0);
	
		success = true;
		error:;
		
		if (fullPIDL) CoTaskMemFree(fullPIDL);
		if (folderPIDL) CoTaskMemFree(folderPIDL);
		if (itemPIDL) CoTaskMemFree(itemPIDL);
		if (persistFolder) persistFolder->Release();
		if (folderView) folderView->Release();
		if (shellView) shellView->Release();
		if (shellBrowser) shellBrowser->Release();
		if (serviceProvider) serviceProvider->Release();
		if (webBrowserApp) webBrowserApp->Release();
		if (dispatch) dispatch->Release();
		
		if (success) break;
	}
	
	shellWindows->Release();
	if (filesInFolder) return 0;
	
	StringCopy(pathBuffer, filePath);
	int lastPathSeparator = 0;
	for (int i = 0; pathBuffer[i]; i++) if (pathBuffer[i] == '\\') lastPathSeparator = i;
	pathBuffer[lastPathSeparator + 1] = '*';
	pathBuffer[lastPathSeparator + 2] = '.';
	pathBuffer[lastPathSeparator + 3] = '*';
	pathBuffer[lastPathSeparator + 4] = 0;
	WIN32_FIND_DATAW entry;
	HANDLE find = FindFirstFileW(pathBuffer, &entry);
	if (find == INVALID_HANDLE_VALUE) return 0;
	do { itemsInFolder++; } while (FindNextFileW(find, &entry));
	FindClose(find);
	if (!(filesInFolder = (FolderEntry *) malloc(itemsInFolder * sizeof(FolderEntry)))) return 0;
	find = FindFirstFileW(pathBuffer, &entry);
	pathBuffer[lastPathSeparator + 1] = 0;
	if (find == INVALID_HANDLE_VALUE) return 0;
	int index = 0;
	
	do { if (index < itemsInFolder) {
		StringCopy(filesInFolder[index].path, pathBuffer);
		StringAppend(filesInFolder[index].path, entry.cFileName); 
		if (StringEqual(filesInFolder[index].path, filePath)) indexInFolder = index;
		index++;
	} } while (FindNextFileW(find, &entry));
	
	FindClose(find);
	itemsInFolder = index;
	PostThreadMessage(threadMain, WM_APP + 1, 0, 0);
	return 0;
}

void WinMainCRTStartup() {
	instance = GetModuleHandle(0);
	processHeap = GetProcessHeap();
	threadMain = GetThreadId(GetCurrentThread());
	
	int argc = 0;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	
	if (argc == 1) {
		clipboardMode = true;
	} else if (argc != 2) {
		MessageBox(0, "No image path specified on command line.", 0, MB_OK);
		ExitProcess(1);
	}
	
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

	{
		GdiplusStartupInput gdiplusStartupInput;
		ULONG_PTR gdiplusToken;
		GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
	}
	
	INITCOMMONCONTROLSEX icc = {};
	icc.dwSize = sizeof(INITCOMMONCONTROLSEX);
	icc.dwICC = ICC_WIN95_CLASSES;
	InitCommonControlsEx(&icc);
		
	if (!clipboardMode) {
		CreateThread(0, 0, GetFolderContents, argv[1], 0, 0);
	}
	
	OSVERSIONINFOEXW version = { sizeof(version) };
	((LONG (*)(PRTL_OSVERSIONINFOEXW)) GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))(&version);

	if (version.dwMajorVersion >= 10) {
		shcore = LoadLibrary("shcore.dll");
		SetProcessDpiAwarenessContextType setProcessDpiAwarenessContext = (SetProcessDpiAwarenessContextType) GetProcAddress(LoadLibrary("user32.dll"), 
			"SetProcessDpiAwarenessContext");
		setProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	} else {
		SetProcessDPIAware();
	}

	WNDCLASSEX windowClass = {};
	windowClass.cbSize = sizeof(WNDCLASSEX);
	windowClass.style = CS_HREDRAW | CS_VREDRAW;
	windowClass.lpfnWndProc = FrameProcedure;
	windowClass.lpszClassName = "frame";
	windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
	windowClass.hIcon = LoadIcon(instance, MAKEINTRESOURCE(1));
	windowFrame = CreateWindowEx(0, (LPSTR) RegisterClassEx(&windowClass), "imgview", 
		WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, 0, 0, 0, 0);
		
	RECT frameBounds;
	GetClientRect(windowFrame, &frameBounds);
	
	cursorPan = LoadCursor(NULL, IDC_HAND);
		
	brushLightBackground = CreateSolidBrush(RGB(238, 243, 250));
	brushDarkBackground = CreateSolidBrush(RGB(23, 26, 30));
	
	windowClass.lpfnWndProc = ViewportProcedure;
	windowClass.lpszClassName = "viewport";
	windowClass.hbrBackground = brushLightBackground;
	windowClass.hIcon = NULL;
	windowViewport = CreateWindowEx(WS_EX_COMPOSITED, (LPSTR) RegisterClassEx(&windowClass), 0, 
		WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, windowFrame, 0, 0, 0);
		
	windowStatusBar = CreateWindowExW(0, STATUSCLASSNAMEW, 0, 
		WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, windowFrame, 0, 0, 0);
	int parts[] = { 200, 400, 600, 800 };
	SendMessage(windowStatusBar, SB_SETPARTS, 4, (LPARAM) parts);
	
	if (!clipboardMode) {
		LoadImage(argv[1]);
		SetWindowTextW(windowFrame, argv[1]);
	} else {
		LoadClipboard();
	}
	
	UpdateStatusBar();
	
	if (imageError) {
		MessageBox(0, "The image could not be loaded.", 0, MB_OK);
		ExitProcess(1);
	}
		
	ShowWindow(windowFrame, SW_MAXIMIZE);
	
	MSG message;
	
	while (GetMessage(&message, NULL, 0, 0)) {
		bool holdCtrl = GetKeyState(VK_CONTROL) & 0x8000;
		if (message.message == WM_APP + 1) {
			filesInFolderReady = true;
		} else if (message.message == WM_KEYDOWN) {
			WPARAM wParam = message.wParam;
			
			if (wParam == 'O' && holdCtrl) {
				SendMessage(windowFrame, WM_COMMAND, 1, 0);
			} else if (wParam == 'C' && holdCtrl) {
				SendMessage(windowFrame, WM_COMMAND, (GetKeyState(VK_SHIFT) & 0x8000) ? 3 : 2, 0);
			} else if (wParam == VK_LEFT) {
				SendMessage(windowFrame, WM_COMMAND, holdCtrl ? 7 : 4, 0);
			} else if (wParam == VK_RIGHT) {
				SendMessage(windowFrame, WM_COMMAND, holdCtrl ? 8 : 5, 0);
			} else if (wParam == VK_F5) {
				SendMessage(windowFrame, WM_COMMAND, 6, 0);
			} else if (wParam == 'R' && holdCtrl) {
				SendMessage(windowFrame, WM_COMMAND, 9, 0);
			} else if (wParam == 'M' && holdCtrl) {
				SendMessage(windowFrame, WM_COMMAND, 10, 0);
			} else if (wParam == 'D' && holdCtrl) {
				SendMessage(windowFrame, WM_COMMAND, 11, 0);
			} else if (wParam == '0' && holdCtrl) {
				SendMessage(windowFrame, WM_COMMAND, 12, 0);
			} else if (wParam == '1' && holdCtrl) {
				SendMessage(windowFrame, WM_COMMAND, 13, 0);
			} else if (wParam == 'L' && holdCtrl) {
				SendMessage(windowFrame, WM_COMMAND, 14, 0);
			} else if (wParam == 'B' && holdCtrl) {
				SendMessage(windowFrame, WM_COMMAND, 15, 0);
			} else {
				goto dispatch;
			}
		} else {
			dispatch:;
			TranslateMessage(&message);
			DispatchMessage(&message);
		}
	}
}