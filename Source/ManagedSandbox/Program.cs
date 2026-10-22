using System.Text;
namespace ManagedSandbox;

class Program
{
    static int Main(string[] args)
    {
        Console.OutputEncoding = Encoding.UTF8;

        Console.WriteLine("自研引擎 C# 验证程序构建成功");
        Console.WriteLine($"当前是否为 64 位进程:{Environment.Is64BitProcess}");

        return 0;
    }
}
