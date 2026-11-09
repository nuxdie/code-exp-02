#include "math_typesetter.h"

#include <chrono>
#include <iostream>
#include <poll.h>
#include <stdexcept>

void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

int main()
{
    try {
        MathTypesetter typesetter;
        const std::string expression = "\\frac{a^2+b^2}{\\sqrt{c}}";
        typesetter.request(expression, true);
        typesetter.request(expression, true);
        typesetter.request("\\notARealTexCommand", false);
        bool success = false, invalid = false;
        int count = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline && count < 2) {
            pollfd fd{typesetter.event_fd(), POLLIN, 0};
            poll(&fd, 1, 100);
            for (const auto& result : typesetter.take_results()) {
                ++count;
                if (result.key == MathTypesetter::key(expression, true))
                    success = result.error.empty() &&
                              result.svg.find("<svg") != std::string::npos &&
                              result.svg.find("<path") != std::string::npos;
                else
                    invalid = !result.error.empty() && result.svg.empty();
            }
        }
        expect(success, "Real LaTeX must render fractions, roots and exponents to vector artwork");
        expect(invalid,
               "Invalid LaTeX must report failure without blocking or crashing the viewer");
        expect(count == 2, "Duplicate equations must be typeset only once");
        std::cout << "Asynchronous LaTeX rendering and error recovery checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
