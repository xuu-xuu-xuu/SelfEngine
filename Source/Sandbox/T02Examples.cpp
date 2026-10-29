#include "T02Examples.h"
#include "Core/Log.h"
#include "Platform/Windows/HResult.h"
#include <Windows.h>
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <cassert>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>


// 通过构造、析构日志观察对象生命周期；id 用于区分对象。
class LifetimeProbe
{
public:
	explicit LifetimeProbe(int id) : id_(id)
	{
		std::cout << "构造对象 #" << id_ << "\n";
	}

	void PrintStatus() const
	{
		std::cout << "正在使用对象 #" << id_ << "\n";
	}

	~LifetimeProbe()
	{
		std::cout << "析构对象 #" << id_ << "\n";
	}

private:
	int id_;

};

// 借用 probe 查看对象状态，不转移所有权。
void InspectProbe(const LifetimeProbe& probe)
{
	probe.PrintStatus();
}

// 批量查看 items 中的对象；span 只借用容器内容。
void InspectProbes(std::span<const std::unique_ptr<LifetimeProbe>> items)
{
	std::cout << "批量借用开始，元素数量：" << items.size() << "\n";

	for (const std::unique_ptr<LifetimeProbe>& item : items)
	{
		InspectProbe(*item);
	}

	std::cout << "批量借用结束\n";
}

enum class ReadFirstLineResult
{
	Success,
	OpenFailed,
	ReadFailed
};

// 读取 filePath 的首行到 outLine，返回读取结果。
ReadFirstLineResult TryReadFirstLine(const std::string& filePath, std::string& outLine)
{
	outLine.clear();

	std::ifstream file(filePath);
	if (!file.is_open())
	{
		return ReadFirstLineResult::OpenFailed;
	}

	if (!std::getline(file, outLine))
	{
		return ReadFirstLineResult::ReadFailed;
	}

	return ReadFirstLineResult::Success;
}

// 演示 ComPtr 共享 COM 对象及自动释放引用。
bool RunComPtrExample()
{
	Microsoft::WRL::ComPtr<ID3DBlob> bufferA;

	// 创建 16 字节缓冲；GetAddressOf 提供接收接口指针的位置。
	const HRESULT createResult =
		D3DCreateBlob(16, bufferA.GetAddressOf());

	if (!CheckHResult(createResult, "创建 COM 内存缓冲区"))
	{
		return false;
	}

	Microsoft::WRL::ComPtr<ID3DBlob> bufferB = bufferA;

	std::cout << "复制后，A 和 B 的接口指针是否相同："
		<< (bufferA.Get() == bufferB.Get()) << '\n';

	bufferA.Reset();

	std::cout << "释放后，A 是否为空："
		<< (bufferA.Get() == nullptr) << '\n';

	std::cout << "释放后，B 是否仍持有对象："
		<< (bufferB.Get() != nullptr) << '\n';

	std::cout << "通过 B 查询到的缓冲区大小："
		<< bufferB->GetBufferSize() << " 字节\n";

	return true;
}

// 将异常转为错误码：返回 0 表示正常，1 表示标准异常，2 表示其他 C++ 异常。
// simulateFailure：1 模拟标准异常，2 模拟整数异常，其他值走正常路径。
extern "C" int Engine_RunBoundaryExample(int simulateFailure) noexcept // 使用 C语言的链接方式 且 不允许异常逃出
{
	try
	{
		LifetimeProbe probe(4);

		if (simulateFailure == 1)
		{
			throw std::runtime_error("模拟接口内部异常");
		}
		else if (simulateFailure == 2)
		{
			throw 42;
		}

		return 0;
	}
	catch (const std::exception&)
	{
		return 1;
	}
	catch (...) // 匹配任意类型的 C++ 异常
	{
		return 2;
	}
}

//T02测试函数
int RunT02Examples()
{

	Log(LogLevel::Info, "进入作用域之前");

	try
	{
		std::unique_ptr<LifetimeProbe> owner = std::make_unique<LifetimeProbe>(1);
		// 移动构造将所有权交给 nextOwner，原 owner 变空。
		std::unique_ptr<LifetimeProbe> nextOwner = std::move(owner);

		assert(nextOwner != nullptr);

		std::cout << std::boolalpha;

		if (!RunComPtrExample())
		{
			return 1;
		}

		std::cout << "owner 是否为空：" << (owner == nullptr) << "\n";
		std::cout << "nextOwner 是否持有对象" << (nextOwner != nullptr) << "\n";

		std::cout << "开始借用\n";
		InspectProbe(*nextOwner);
		std::cout << "借用结束，nextOwner 是否持有对象：" << (nextOwner != nullptr) << "\n";

		std::vector<std::unique_ptr<LifetimeProbe>> probes;
		probes.push_back(std::move(nextOwner));
		probes.push_back(std::make_unique<LifetimeProbe>(2));
		probes.push_back(std::make_unique<LifetimeProbe>(3));
		std::cout << "容器中元素数量：" << probes.size() << "\n";
		std::cout << "nextOwner 是否为空：" << (nextOwner == nullptr) << "\n";

		InspectProbes(probes);
		std::cout << "批量借用后，容器元素数量：" << probes.size() << "\n";

		const std::string filePath = "README.md";
		std::string firstLine;

		const ReadFirstLineResult result = TryReadFirstLine(filePath, firstLine);

		switch (result)
		{
		case ReadFirstLineResult::Success:
			break;

		case ReadFirstLineResult::OpenFailed:
			Log(LogLevel::Warning, "打开文件失败，使用默认文字，路径：" + filePath);
			firstLine = "默认内容";
			break;

		case ReadFirstLineResult::ReadFailed:
			Log(LogLevel::Warning, "读取第一行失败，使用默认文字，路径：" + filePath);
			firstLine = "默认内容";
			break;
		}

		std::cout << "文件第一行:" << firstLine << '\n';

		const int boundaryResult = Engine_RunBoundaryExample(0);
		std::cout << "C 接口返回码:" << boundaryResult << "\n";
		if (boundaryResult != 0)
		{
			Log(LogLevel::Error, "C 接口报告失败，提前退出");
			return 1;
		}
	}
	catch (const std::exception& error)
	{
		Log(LogLevel::Error, std::string("捕获异常：") + error.what());
		return 1;
	}

	Log(LogLevel::Info, "离开作用域之后");

	return 0;
}