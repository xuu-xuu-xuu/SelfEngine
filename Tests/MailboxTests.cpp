#include "CoreLayer/BoundedMailbox.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
struct Message {std::uint64_t sequence=0;std::vector<std::uint8_t> bytes;};
void Check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
}
int main(int argc,char** argv)
{
    try
    {
        using namespace se;
        BoundedMailbox<Message> mailbox(2,8);const auto publisher=mailbox.GetPublisher();
        Check(publisher.Push({1,{1,2,3,4}},4)==MailboxResult::Accepted&&publisher.Push({2,{5,6,7,8}},4)==MailboxResult::Accepted,"owned messages accepted");
        auto batch=mailbox.TakeIf([](const Message& value){return value.sequence==1;});
        Check(batch.size()==1&&batch[0].Get().bytes[0]==1&&mailbox.Statistics().queued==1&&mailbox.Statistics().pending==2,"take does not release in-flight budget");
        Check(publisher.Push({3,{}},0)==MailboxResult::Full,"in-flight message counts toward capacity");
        batch[0].Reset();Check(publisher.Push({3,{9}},1)==MailboxResult::Accepted,"release restores budget");
        Check(publisher.Push({4,{}},9)==MailboxResult::TooLarge,"per-item byte overflow explicit");
        auto rest=mailbox.TakeIf([](const Message&){return true;});Check(rest.size()==2&&rest[0].Get().sequence==2&&rest[1].Get().sequence==3,"selection keeps producer order");
        mailbox.Close();Check(publisher.Push({5,{}},0)==MailboxResult::Closed&&mailbox.Statistics().pending==2,"close rejects late messages while leases remain valid");
        rest.clear();Check(mailbox.Statistics().pending==0&&mailbox.Statistics().bytes==0,"all lease budgets released");
        BoundedMailbox<Message>::Publisher late;
        {BoundedMailbox<Message> owner(1,1);late=owner.GetPublisher();Check(late.Push({1,{1}},1)==MailboxResult::Accepted,"temporary owner");}
        Check(late.Push({2,{2}},1)==MailboxResult::Closed,"weak publisher cannot resurrect owner");
        BoundedMailbox<Message> threaded(256,256);const auto worker=threaded.GetPublisher();bool accepted=true;
        std::thread producer([&]{for(std::uint64_t i=0;i<128;++i)if(worker.Push({i,{std::uint8_t(i)}},1)!=MailboxResult::Accepted)accepted=false;});producer.join();
        Check(accepted,"worker only publishes owned data");auto ordered=threaded.TakeIf([](const Message&){return true;});
        Check(ordered.size()==128,"all worker messages received");for(std::size_t i=0;i<ordered.size();++i)Check(ordered[i].Get().sequence==i&&ordered[i].Get().bytes[0]==i,"same producer FIFO");
        ordered.clear();Check(threaded.Statistics().pending==0&&threaded.Statistics().peakPending==128,"threaded budget trace");
        const std::filesystem::path proof=argc>1?argv[1]:"out/mailbox/verification.json";std::filesystem::create_directories(proof.parent_path());
        std::ofstream(proof)<<nlohmann::json{{"success",true},{"ownedPayload",true},{"inFlightBudget",true},{"byteBudget",true},{"fifo",true},{"closedLateRejected",true},{"weakPublisher",true},{"workerMessages",128},{"pendingAfterRelease",0}}.dump(2);
        std::cout<<"Mailbox passed (owned batches, in-flight budget, weak close and worker FIFO)\n";return 0;
    }
    catch(const std::exception& exception){std::cerr<<"[FAIL] "<<exception.what()<<'\n';return 1;}
}
