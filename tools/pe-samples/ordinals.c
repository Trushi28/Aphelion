__declspec(dllimport) int __stdcall Alpha(int);
__declspec(dllimport) int __stdcall Beta(int);
__declspec(dllimport) int __stdcall Gamma(int);
__declspec(dllimport) void __stdcall ExitProcess(unsigned);
void mainCRTStartup(void) {
    ExitProcess(Alpha(1) + Beta(2) + Gamma(3));
}
