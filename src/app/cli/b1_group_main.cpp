#include "app/AppPaths.h"
#include "app/B1Group.h"

int spirula_train_blocks_main(int argc, char** argv) {
    return app::run_b1_group(argc, argv, app::exe_path(), app::config_dir());
}
