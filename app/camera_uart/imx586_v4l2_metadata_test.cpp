#include "imx586_v4l2_metadata.h"

#include <iostream>

int main()
{
    if (!imx586_v4l2::self_test()) {
        std::cerr << "IMX586_V4L2_METADATA_TEST_FAILED\n";
        return 1;
    }
    std::cout << "IMX586_V4L2_METADATA_TEST_OK\n";
    return 0;
}
