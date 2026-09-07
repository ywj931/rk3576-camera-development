#include "gnss_time_source.h"

#include <cstdio>
#include <cstdlib>

int main()
{
    char report[256] = {};
    const int result = gnss_time_source_self_test(report, sizeof(report));
    if (result != GNSS_TIME_OK) {
        std::fprintf(stderr,
                     "GNSS_TIME_SOURCE_TEST_FAILED code=%d reason=\"%s\" detail=\"%s\"\n",
                     result, gnss_time_source_strerror(result), report);
        return EXIT_FAILURE;
    }
    std::printf("GNSS_TIME_SOURCE_TEST_OK detail=\"%s\"\n", report);
    return EXIT_SUCCESS;
}
