#include "conan/shutdown.h"

namespace conan {

std::vector<int> CountdownMarks(int total) {
    std::vector<int> out;
    if (total <= 0) return out;
    out.push_back(total);
    for (int m : {300, 240, 180, 120, 60, 30, 10, 5, 4, 3, 2, 1})
        if (m < total) out.push_back(m);
    return out;
}

std::string CountdownText(int s) {
    if (s >= 60 && s % 60 == 0) {
        int m = s / 60;
        return "The server shuts down in " + std::to_string(m) + (m == 1 ? " minute." : " minutes.");
    }
    return "The server shuts down in " + std::to_string(s) + (s == 1 ? " second." : " seconds.");
}

}  // namespace conan
