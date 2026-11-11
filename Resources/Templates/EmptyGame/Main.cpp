#include "ToolLayer/Tools/Project.h"
#include <iostream>
int wmain(int argc,wchar_t** argv)
{
    const std::filesystem::path manifest=argc>1?argv[1]:L"project.json";
    const std::filesystem::path output=argc>2?argv[2]:L"out/replay";
    se::ReplayReport report;std::vector<se::ProjectDiagnostic> diagnostics;
    if(!se::ReplayProject(manifest,output,report,diagnostics))
    {for(const auto& diagnostic:diagnostics)std::cerr<<diagnostic.key<<": "<<diagnostic.detail<<'\n';return 1;}
    std::cout<<"steps="<<report.steps<<" stateSha256="<<report.stateHash<<'\n';return 0;
}
