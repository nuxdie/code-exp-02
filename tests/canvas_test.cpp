#include "canvas.h"

#include <cstdlib>
#include <iostream>

void expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

int main()
{
    expect(arrange_files({}).empty(), "Empty projects must have an empty layout");
    const auto single = arrange_files({{800, 400}});
    expect(single.size() == 1 && single[0].origin.x == 0 && single[0].origin.y == 0,
           "One file must start at the canvas origin");

    const std::vector<Vec2> sizes = {{1500, 14000}, {900, 3200},  {820, 1800}, {1100, 1500},
                                     {700, 600},    {1300, 5200}, {900, 1600}, {600, 800}};
    const auto files = arrange_files(sizes);
    expect(files.size() == sizes.size(), "Every source file must have a panel");
    expect(files[0].origin.x < files[1].origin.x && files[1].origin.x < files[2].origin.x,
           "The first files must occupy separate columns");
    expect(files[3].origin.x == files[2].origin.x && files[3].origin.y > 0,
           "The next file must stack in the shortest column");
    const auto bounds = canvas_size(files);
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& file = files[i];
        expect(file.size.x == sizes[i].x && file.size.y == sizes[i].y,
               "Layout must preserve each file's measured dimensions");
        expect(file.origin.x >= 0 && file.origin.y >= 0 &&
                   file.origin.x + file.size.x <= bounds.x &&
                   file.origin.y + file.size.y <= bounds.y,
               "Camera bounds must contain every file");
        for (std::size_t j = i + 1; j < files.size(); ++j)
            expect(!file.intersects(files[j]), "Source panels must never overlap");
    }
    expect(CanvasRect{{10, 20}, {50, 60}}.intersects({{20, 30}, {5, 5}}),
           "Visible files must pass viewport culling");
    expect(!CanvasRect{{10, 20}, {50, 60}}.intersects({{60, 20}, {5, 5}}),
           "Files outside the viewport must be culled");
    std::cout << "File arrangement, spacing, canvas bounds and culling checks passed.\n";
}
