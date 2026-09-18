#include "app/TrainerCore.h"

#include <cmath>
#include <cstdio>

using spirula::RunState;
using spirula::build_step_config;
using spirula::scheduled_lr;

int main() {
    int failures = 0;
    auto near = [&](float got, float expected, const char* name) {
        if (!std::isfinite(got) || std::abs(got - expected) > 1e-6f * std::max(1.0f, std::abs(expected))) {
            std::printf("FAIL %s: got %.9g expected %.9g\n", name, got, expected);
            ++failures;
        }
    };
    TrainConfig c;
    c.num_iterations = 100;
    c.max_steps = 100;
    c.means_lr = 1.0f;
    c.means_lr_final = 0.01f;
    c.use_scale_agnostic_mean = false;
    c.scale_reg = 2.0f;
    c.scale_reg_decay_power = 0.0f;
    c.reg_warmup_length = 10;
    c.supervision_warmup = 20;
    c.distortion_reg_warmup = 40;
    c.median_warmup = 30;
    c.normal_reg_weight = 2.0f;
    c.depth_supervision_weight = 3.0f;
    c.mean_median_depth_weight = 4.0f;
    RunState state;
    state.train_frame_scale = 2.0f;
    for (int step : {0, 9, 10, 11, 19, 20, 21, 29, 30, 31, 39, 40, 41, 50, 99, 100, 101}) {
        const auto config = build_step_config(c, state, step);
        near(config.optim.lr_means, 2.0f * std::pow(0.01f, std::min(step, 100) / 100.0f),
             "absolute-step mean learning rate including resumed midpoint");
        near(config.optim.mcmc_scale_reg_weight, 1.0f, "world scale normalization");
        near(config.loss.weights[(int)LossWeightIndex::NormalReg],
             step < 10 ? 0.0f : 2.0f * std::min(step, 40) / 40.0f,
             "regularization inclusive activation and distortion ramp");
        near(config.loss.weights[(int)LossWeightIndex::DepthSup], step <= 20 ? 0.0f : 3.0f,
             "supervision activates strictly after warmup");
        near(config.loss.weights[(int)LossWeightIndex::MeanMedianDepthSup],
             4.0f * std::min(step, 30) / 30.0f, "median ramp saturates at boundary");
    }
    near(scheduled_lr(0, 100, 1.0f, {}, 10), 0.0f, "warmup starts at zero");
    near(scheduled_lr(9, 100, 1.0f, {}, 10), 0.9f, "warmup penultimate step");
    near(scheduled_lr(10, 100, 1.0f, {}, 10), 1.0f, "warmup endpoint");
    near(scheduled_lr(11, 100, 1.0f, {}, 10), 1.0f, "warmup remains saturated");
    c.use_scale_agnostic_mean = true;
    near(build_step_config(c, state, 50).optim.lr_means, 0.1f,
         "scale-agnostic means do not inherit scene scale");
    std::printf("%s training schedule boundaries\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
