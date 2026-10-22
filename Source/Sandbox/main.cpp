#include <iostream>
#include<Windows.h>

int main()
{
	static_assert(sizeof(void*) == 8, "自研引擎需要64位编译器"); // 检查当前构建 指针大小是否为8位
	SetConsoleOutputCP(CP_UTF8);
	std::cout << "自研引擎第一次构建成功\n";
	return 0;
}