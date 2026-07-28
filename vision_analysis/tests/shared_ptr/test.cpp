#include <memory>
#include <stdio.h>

struct ChannelContext
{
    int target_count = 0;
};

int main()
{
    std::shared_ptr<void> state = std::make_shared<ChannelContext>();
    
}
