#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00

// credits to arpruss

#include <Windows.h>
#include <hidsdi.h>
#include <time.h>
#include <stdio.h>
#include <stdbool.h>
#include <math.h>

#define OUT_BUFFER_SIZE 4096
#define IN_BUFFER_SIZE  4096

enum {
	SHIFT_MODE_NONE = 0,
	SHIFT_MODE_LIFT,
	SHIFT_MODE_DOWN
};

int shiftMode = SHIFT_MODE_LIFT;
unsigned twofinger_detect_delay_clocks = CLOCKS_PER_SEC * 50 / 1000;
unsigned click_detect_delay_clocks = CLOCKS_PER_SEC * 50 / 1000;
HHOOK miHook;
clock_t last_click = 0;
clock_t last_problem_twofinger_time = 0;
bool touching = false;
int drawX;
int drawY;
float aspectRatio;

HWND overlayWnd = NULL;

INPUT outBuffer[OUT_BUFFER_SIZE];
unsigned outBufferHead;
unsigned outBufferTail;

#define KEY_MAIN_ACTIVATE_MOD  0x1D // ctrl
#define KEY_QUICK_ACTIVATE_MOD 0x38 // alt
#define KEY_ACTIVATE		   0x5B // super
#define KEY_LEFT_SHIFT 0x2a
#define KEY_RIGHT_SHIFT 0x36

bool downMainMod = false;
bool downQuickMod = false;
bool downActivate = false;
bool downShift = false;

#define INPUT_WAIT 1234

enum {
	MODE_NONE = 0,
	MODE_FIRST_CORNER = 1,
	MODE_ACTIVE = 2
};

int mode = 0;
int mouseX = 0;
int mouseY = 0;
int topLeftX;
int topLeftY;
int bottomRightX;
int bottomRightY;

volatile bool running = true;

HANDLE queueReady;

int popBuffer(INPUT* ip) {
    if (outBufferHead == outBufferTail)
        return -1;
    *ip = outBuffer[outBufferHead];
    outBufferHead = (outBufferHead+1) % OUT_BUFFER_SIZE;
    return 0;
}

int pushBuffer(INPUT* i) {
    unsigned newTail = (outBufferTail+1) % OUT_BUFFER_SIZE;
    if (newTail == outBufferHead)
        return -1;
    outBuffer[outBufferTail] = *i;
    outBufferTail = newTail;
    SetEvent(queueReady);
    return 0;
}

DWORD WINAPI handleQueue(void* arg) {
	INPUT i;
    while(running) {
        WaitForSingleObject(queueReady, INFINITE);
        while (running && popBuffer(&i) >= 0) {
			if (i.type == INPUT_WAIT) {
				Sleep(i.hi.uMsg);
			}
			else {
				SendInput(1,&i,sizeof(INPUT));
			}
        }
    }
    
    ExitThread(0);
    return 0;
}

DWORD readRegistry(HKEY key, char* path, char* value, DWORD defaultValue) {
	DWORD dataSize = {0};
	DWORD out;
	DWORD length = sizeof(DWORD);
    LONG result = RegGetValue(
        key,
        path, 
        value,        // Value
        RRF_RT_DWORD,  // Flags, REG_SZ
        NULL,              
        &out,              // Data, empty for now
        &length);     // Getting the size only 
	if (ERROR_SUCCESS != result)
		return defaultValue;
	else
		return out;
}

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    static char remapped_down = 0;
    if(nCode == HC_ACTION) {
		
		MSLLHOOKSTRUCT* p = (MSLLHOOKSTRUCT*)lParam;
		
		if (wParam == WM_MOUSEMOVE) {
			mouseX = p->pt.x;
			mouseY = p->pt.y;

			// we make rectangle go movey movey
			if (mode == MODE_FIRST_CORNER) {
				int rawWidth  = mouseX - topLeftX;
				int rawHeight = mouseY - topLeftY;

				int absWidth  = abs(rawWidth);
				int absHeight = abs(rawHeight);

				if (absHeight < 5) absHeight = 5;
				if (absWidth < 5) absWidth = 5;

				// preserve aspect ratio
				if (absWidth / (float)absHeight > aspectRatio)
					absWidth = (int)(absHeight * aspectRatio + 0.5f);
				else
					absHeight = (int)(absWidth / aspectRatio + 0.5f);

				rawWidth  = (rawWidth < 0) ? -absWidth : absWidth;
				rawHeight = (rawHeight < 0) ? -absHeight : absHeight;

				int x2 = topLeftX + rawWidth;
				int y2 = topLeftY + rawHeight;

				int finalLeft   = min(topLeftX, x2);
				int finalRight  = max(topLeftX, x2);
				int finalTop    = min(topLeftY, y2);
				int finalBottom = max(topLeftY, y2);

				if (overlayWnd) {
					SetWindowPos(
						overlayWnd,
						HWND_TOPMOST,
						finalLeft,
						finalTop,
						finalRight - finalLeft,
						finalBottom - finalTop,
						SWP_SHOWWINDOW
					);
				}
			}
		}

		if (p->flags & LLMHF_INJECTED) {
		}
		else {
			if (mode == MODE_ACTIVE)
				return 1;
		}
    }
 
    return CallNextHookEx(miHook, nCode, wParam, lParam); // Important! Otherwise other mouse hooks may misbehave
}

int haveValueCap(HIDP_VALUE_CAPS* cap, unsigned usagePage, unsigned usage) {
	if (cap->UsagePage != usagePage)
		return 0;
	if (cap->IsRange) {
		return cap->Range.UsageMin <= usage && usage <= cap->Range.UsageMax;
	}
	else {
		return cap->NotRange.Usage == usage;
	}
}

int haveButtonCap(HIDP_BUTTON_CAPS* cap, unsigned usagePage, unsigned usage) {
	if (cap->UsagePage != usagePage)
		return 0;
	if (cap->IsRange) {
		return cap->Range.UsageMin <= usage && usage <= cap->Range.UsageMax;
	}
	else {
		return cap->NotRange.Usage == usage;
	}
}

long getScaled(unsigned scale, unsigned usagePage, unsigned usage, unsigned scaleUsagePage, unsigned scaleUsage, PHIDP_PREPARSED_DATA preparsed, unsigned char* data, unsigned dataSize) {
	long x;
	long res = HidP_GetUsageValue(HidP_Input, usagePage, 0, usage, &x, preparsed, data, dataSize);
	if (res < 0)
		return -1;
	static HIDP_VALUE_CAPS cap[IN_BUFFER_SIZE / sizeof(HIDP_VALUE_CAPS)];
	SHORT length = sizeof(cap)/sizeof(HIDP_VALUE_CAPS);
	res = HidP_GetSpecificValueCaps(HidP_Input, scaleUsagePage, 0, scaleUsage, cap, &length, preparsed);
	if (res < 0)
		return -1;
	if (cap[0].LogicalMax <= cap[0].LogicalMin)
		return -1;
	
	int range = cap[0].LogicalMax-cap[0].LogicalMin;
	return (scale * (x-cap[0].LogicalMin) + range/2) / range;
}

void moveMouse(int x, int y) {
	INPUT ip;
    ip.type = INPUT_MOUSE;
    ip.mi.dx = x*65536/GetSystemMetrics(SM_CXSCREEN);
    ip.mi.dy = y*65536/GetSystemMetrics(SM_CYSCREEN);
    ip.mi.mouseData = 0;
	ip.mi.dwFlags = MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE;
    ip.mi.time = 0;
	pushBuffer(&ip);
}

void pressMouse(int down) {
	INPUT ip;
    ip.type = INPUT_MOUSE;
    ip.mi.dx = 0;
    ip.mi.dy = 0;
    ip.mi.mouseData = 0;
	ip.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    ip.mi.time = 0;
	pushBuffer(&ip);
}

void delay(unsigned ms) {
	INPUT ip;
    ip.type = INPUT_WAIT;
	ip.hi.uMsg = ms;
	pushBuffer(&ip);
}

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

		RECT r;
        GetClientRect(hwnd, &r);
        HBRUSH clearBrush = CreateSolidBrush(RGB(0,0,0));
        FillRect(hdc, &r, clearBrush);
        DeleteObject(clearBrush);

        if (mode == MODE_FIRST_CORNER) {
            HBRUSH fillBrush = CreateSolidBrush(RGB(138, 186, 25));
            FillRect(hdc, &ps.rcPaint, fillBrush);
            DeleteObject(fillBrush);
        }

        HPEN borderPen = CreatePen(PS_SOLID, 3, RGB(138, 186, 25));
        HGDIOBJ oldPen = SelectObject(hdc, borderPen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH)); // no brush for border
        Rectangle(hdc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right, ps.rcPaint.bottom);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(borderPen);

        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProc(hwnd, msg, w, l);
}



void showActivate(int state, int ms) {
	pressMouse(0);

	if (overlayWnd) {
        if (state) {
			ShowWindow(overlayWnd, SW_HIDE);
            SetWindowPos(
                overlayWnd,
                HWND_TOPMOST,
                topLeftX,
                topLeftY,
                bottomRightX - topLeftX,
                bottomRightY - topLeftY,
                SWP_SHOWWINDOW
            );
        } else {
            ShowWindow(overlayWnd, SW_HIDE);
        }
    }

	if(state)
		SetConsoleTitle("Drawing mode active");
		//puts("Activating drawing mode");
	else
		SetConsoleTitle("winprecision-drawing");
		//puts("Disabling drawing mode");
	int w = bottomRightX-topLeftX;
	int h = bottomRightY-topLeftY;
	if (state) {
		moveMouse((topLeftX+bottomRightX) / 2, (topLeftY+bottomRightY) / 2);
		/*moveMouse(topLeftX, topLeftY);
		delay(ms);
		moveMouse(bottomRightX, topLeftY);
		delay(ms);
		moveMouse(bottomRightX, bottomRightY);
		delay(ms);
		moveMouse(topLeftX, bottomRightY);
		delay(ms);
		moveMouse(topLeftX, topLeftY);*/
	}
	/*else {
		moveMouse(bottomRightX, bottomRightY);
		delay(ms);
		moveMouse(bottomRightX, topLeftY);
		delay(ms);
		moveMouse(topLeftX, topLeftY);
		delay(ms);
		moveMouse(topLeftX, bottomRightY);
		delay(ms);
		moveMouse(bottomRightX, bottomRightY);
	}*/
}

void handleKeyboard(USHORT code, USHORT flags) {
	bool down = ! (flags & 1);
	if (code == KEY_MAIN_ACTIVATE_MOD) 
		downMainMod = down;
	else if (code == KEY_QUICK_ACTIVATE_MOD)
		downQuickMod = down;
	else if (code == KEY_LEFT_SHIFT || code == KEY_RIGHT_SHIFT)
		downShift = down;
	else if (code == KEY_ACTIVATE) {
		downActivate = down;
		if (downActivate && downMainMod) {
			switch(mode) {
				case MODE_NONE:
					topLeftX = mouseX;
					topLeftY = mouseY;
					mode = MODE_FIRST_CORNER;
					SetConsoleTitle("Waiting for 2nd corner...");
					//puts("Press ctrl+win to define other corner and enter drawing mode.");
					break;
				case MODE_FIRST_CORNER:
					{
						int rawWidth  = mouseX - topLeftX;
						int rawHeight = mouseY - topLeftY;

						int absWidth  = abs(rawWidth);
						int absHeight = abs(rawHeight);
						
						if (absHeight < 5) absHeight = 5;
						if (absWidth < 5) absWidth = 5;

						if (absWidth / (float)absHeight > aspectRatio)
							absWidth = (int)(absHeight * aspectRatio + 0.5f);
						else
							absHeight = (int)(absWidth / aspectRatio + 0.5f);

						rawWidth  = (rawWidth < 0) ? -absWidth : absWidth;
						rawHeight = (rawHeight < 0) ? -absHeight : absHeight;

						int x2 = topLeftX + rawWidth;
						int y2 = topLeftY + rawHeight;

						int finalLeft   = min(topLeftX, x2);
						int finalRight  = max(topLeftX, x2);
						int finalTop    = min(topLeftY, y2);
						int finalBottom = max(topLeftY, y2);

						topLeftX     = finalLeft;
						bottomRightX = finalRight;
						topLeftY     = finalTop;
						bottomRightY = finalBottom;

						mode = MODE_ACTIVE;
						touching = false;
						showActivate(1,50);
						drawX = -1;
						drawY = -1;
					}
					break;
				case MODE_ACTIVE:
					//printf("area: (%d,%d)-(%d,%d)\n",topLeftX,topLeftY,bottomRightX,bottomRightY);
					showActivate(0,50);
					mode = MODE_NONE;
					//SetConsoleTitle("winprecision-drawing");
					//puts("Press ctrl+win to define first corner or alt+win to return to previous area.");
					break;
			}
		}
		else if (downActivate && downQuickMod) {
			if (mode == MODE_ACTIVE) {				
				showActivate(0,50);
				mode = MODE_NONE;				
			}
			else {
				//printf("area: (%d,%d)-(%d,%d)\n",topLeftX,topLeftY,bottomRightX,bottomRightY);
				mode = MODE_ACTIVE;
				touching = false;
				showActivate(1,50);
				drawX = -1;
				drawY = -1;
			}
		}
	}
	
}

LRESULT CALLBACK EventHandler(
    HWND hwnd,
    unsigned event,
    WPARAM wparam,
    LPARAM lparam
) {
    static BYTE rawinputBuffer[sizeof(RAWINPUT)+IN_BUFFER_SIZE];
	static BYTE preparsedBuffer[IN_BUFFER_SIZE];
	static BYTE usageBuffer[IN_BUFFER_SIZE];
	USAGE* usages = (USAGE*)usageBuffer;
	RAWINPUT* data = (RAWINPUT*)rawinputBuffer;
	PHIDP_PREPARSED_DATA preparsed = (PHIDP_PREPARSED_DATA)preparsedBuffer;

    switch (event) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_INPUT: {
			unsigned size = sizeof(rawinputBuffer);
            int res = GetRawInputData((HRAWINPUT)lparam, RID_INPUT, data, &size, sizeof(RAWINPUTHEADER));
            if (res < 0 || size == 0)
				return 0;
			if (data->header.dwType == RIM_TYPEKEYBOARD) {
				handleKeyboard(data->data.keyboard.MakeCode,data->data.keyboard.Flags);
				return 0;
			}
			if (mode != MODE_ACTIVE)
				return 0;
			size = sizeof(preparsedBuffer);
			res = GetRawInputDeviceInfo(data->header.hDevice, RIDI_PREPARSEDDATA, preparsed, &size);
			if (res < 0 || size == 0) 
				return 0;
			
			unsigned long usageLength = sizeof(usageBuffer)/sizeof(USAGE);
			bool touch = false;
			
			res = HidP_GetUsages(HidP_Input, 0x0D, 0, usages, &usageLength, preparsed, data->data.hid.bRawData, data->data.hid.dwSizeHid);
			if (res < 0)
				return 0;
			
			for (int j=0;j<usageLength;j++) {
				if (usages[j]==0x42) {
					touch = true;
					break;
				}
			}
			
			if (shiftMode == SHIFT_MODE_DOWN)
				touch = touch && downShift;
			else if (shiftMode == SHIFT_MODE_LIFT)
				touch = touch && !downShift;

			int x,y;
			x = getScaled(bottomRightX-topLeftX, 0x01, 0x30, 0x01, 0x30, preparsed, data->data.hid.bRawData, data->data.hid.dwSizeHid);
			if (x>=0) {
				y = getScaled(bottomRightY-topLeftY, 0x01, 0x31, 0x01, 0x31, preparsed, data->data.hid.bRawData, data->data.hid.dwSizeHid);
				if (y>=0) {
					x+=topLeftX;
					y+=topLeftY;
					if (x!=drawX || y!=drawY) {
						moveMouse(x,y);
						drawX = x;
						drawY = y;
					}
				}
			} 
			if (touch != touching) {
				pressMouse(touch);
				touching = touch;
			}	
        } return 0;
    }

    return DefWindowProc(hwnd, event, wparam, lparam);
}

void help() {
	puts("winprecision-drawing [option]\n"
	"\n"\
	"Options:\n"\
	"--help / -h     : List the available options.\n"\
	"--shift-penup   : Raise pen when [SHIFT] is used. (Default)\n"\
	"--shift-pendown : Lower pen when [SHIFT] is used.\n"\
	"--shift-none    : [SHIFT] is ignored.\n");
}

int processOptions(char* cmdLine) {
	char* token;
	char* src;
	src = cmdLine;

	while (NULL != (token = strtok(src, " "))) {
		src = NULL;
		if (!strcmp(token, "--shift-penup")) {
			shiftMode = SHIFT_MODE_LIFT;
		}
		else if (!strcmp(token, "--shift-pendown")) {
			shiftMode = SHIFT_MODE_DOWN;
		}
		else if (!strcmp(token, "--shift-none")) {
			shiftMode = SHIFT_MODE_NONE;
		}
		else if (!strcmp(token, "--help") || !strcmp(token, "-h")) {
			help();
			return 0;
		}
	}
	return 1;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE hPrevInstance, PSTR lpCmdLine, int nCmdShow) {
    const char* class_name = "winprecision-drawing-class";
	
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
	int screenWidth = GetSystemMetrics(SM_CXSCREEN);
	int screenHeight = GetSystemMetrics(SM_CYSCREEN);
	
	topLeftX = 0;
	topLeftY = 0;
	bottomRightX = screenWidth;
	bottomRightY = screenHeight;

	if (!processOptions(lpCmdLine))
		return 0;
	
    //HINSTANCE instance = GetModuleHandle(0);
    WNDCLASS window_class = {};
    window_class.lpfnWndProc = EventHandler;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;

    if (!RegisterClass(&window_class))
        return -1;

    HWND window = CreateWindow(class_name, "winprecision-drawing-overlay", 0, 0, 0, 0, 0, HWND_MESSAGE, 0, 0, 0);
	
    if (window == NULL)
        return -1;
	
	// Overlay class
    WNDCLASS overlayClass = {0};
    overlayClass.lpfnWndProc = OverlayProc;
    overlayClass.hInstance = instance;
    overlayClass.lpszClassName = "winprecision-drawing-overlay";
    RegisterClass(&overlayClass);

    // Overlay window
    overlayWnd = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        "winprecision-drawing-overlay",
        NULL,
        WS_POPUP,
        topLeftX, topLeftY,
        bottomRightX - topLeftX,
        bottomRightY - topLeftY,
        NULL, NULL, instance, NULL
    );
    SetLayeredWindowAttributes(overlayWnd, 0, 80, LWA_ALPHA);
    ShowWindow(overlayWnd, SW_HIDE);
	
	SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);

    RAWINPUTDEVICE rid[2];
	//touchpad
    rid[0].usUsagePage = 0x0D; 
    rid[0].usUsage = 0x05;
    rid[0].dwFlags = RIDEV_INPUTSINK;
    rid[0].hwndTarget = window;
	//keyboard
    rid[1].usUsagePage = 0x01;
    rid[1].usUsage = 0x06;
    rid[1].dwFlags = RIDEV_INPUTSINK;
    rid[1].hwndTarget = window;
    int res = RegisterRawInputDevices(rid, 2, sizeof(rid[0]));

    miHook = SetWindowsHookEx(WH_MOUSE_LL, (HOOKPROC)(&LowLevelMouseProc), 0, 0);

    queueReady = CreateEvent(NULL, FALSE, FALSE, (LPTSTR)("queueReady"));
    HANDLE queueThread = CreateThread(NULL, 0, handleQueue, NULL, 0, NULL);

	// get touchpad aspect ratio
	{
		UINT nDevices;
		GetRawInputDeviceList(NULL, &nDevices, sizeof(RAWINPUTDEVICELIST));
		if (nDevices == 0) goto aspect_ratio_done;

		RAWINPUTDEVICELIST* deviceList = malloc(sizeof(RAWINPUTDEVICELIST) * nDevices);
		if (!deviceList) goto aspect_ratio_done;

		GetRawInputDeviceList(deviceList, &nDevices, sizeof(RAWINPUTDEVICELIST));

		for (UINT i = 0; i < nDevices; i++) {
			if (deviceList[i].dwType == RIM_TYPEHID) {
				RID_DEVICE_INFO info = {0};
				info.cbSize = sizeof(info);
				UINT size = sizeof(info);
				GetRawInputDeviceInfo(deviceList[i].hDevice, RIDI_DEVICEINFO, &info, &size);

				if (info.hid.usUsagePage == 0x0D && info.hid.usUsage == 0x05) {
					PHIDP_PREPARSED_DATA preparsed;
					size = 0;
					GetRawInputDeviceInfo(deviceList[i].hDevice, RIDI_PREPARSEDDATA, NULL, &size);

					preparsed = malloc(size);
					if (!preparsed) break;

					GetRawInputDeviceInfo(deviceList[i].hDevice, RIDI_PREPARSEDDATA, preparsed, &size);

					HIDP_CAPS caps;
					if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS) {
						HIDP_VALUE_CAPS capsX[10], capsY[10];
						USHORT lenX = 10, lenY = 10;
						HidP_GetSpecificValueCaps(HidP_Input, 0x01, 0, 0x30, capsX, &lenX, preparsed);
						HidP_GetSpecificValueCaps(HidP_Input, 0x01, 0, 0x31, capsY, &lenY, preparsed);

						int tpWidth  = capsX[0].LogicalMax - capsX[0].LogicalMin + 1;
						int tpHeight = capsY[0].LogicalMax - capsY[0].LogicalMin + 1;
						aspectRatio = tpWidth / (float)tpHeight;
					}

					free(preparsed);
					break;
				}
			}
		}

		free(deviceList);
	}
	aspect_ratio_done: // when something fails abovee we use this and fallback on default values

	SetConsoleTitle("winprecision-drawing");

	printf("-- winprecision-drawing --\n"
		 "Use Precision Touchpads as a drawing pad for handwriting, signatures, art, and more.\nOriginal code by arpruss // Adapted for ease of use by mham-z\n\n"
		 "Enter drawing mode by using [CTRL] + [SUPER] to mark 2 points of the area you want to draw in.\n"
		 "Use [ALT] + [SUPER] to enter drawing mode using the last area you marked.\n");
	if (shiftMode == SHIFT_MODE_LIFT)
		printf("Hold [SHIFT] for pen up (release [LMB]) when in drawing mode.\n");
	else if (shiftMode == SHIFT_MODE_DOWN)
		printf("Hold [SHIFT] for pen down (depress [LMB]) when in drawing mode.\n");

    MSG message;
    while(GetMessage(&message, NULL, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessage(&message);
    }

    running = false;

    SetEvent(queueReady);
    if (miHook) UnhookWindowsHookEx(miHook);

	CloseHandle(queueThread);
	CloseHandle(queueReady);

    return 0;
}