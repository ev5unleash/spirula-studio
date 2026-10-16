#include <checkpoint/B1Merge.h>

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: " << argv[0] << " <left.ply> <right.ply> <merged.ply>\n";
        return 2;
    }
    try {
        ckpt::merge_b1_ply(argv[1], argv[2], argv[3]);
        std::cout << "b1_merge: published " << argv[3] << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "b1_merge: " << e.what() << '\n';
        return 1;
    }
}
