typedef unsigned long long u64;
int counter = 3;
int* where = &counter;
__declspec(dllexport) int __stdcall Twice(int v) { return v * 2 + *where; }
__declspec(dllexport) int __stdcall Thrice(int v) { return v * 3; }
int __stdcall DllMainCRTStartup(void* h, unsigned reason, void* r) { return 1; }
