// Actual DF DenseObs assembly and LM solves against the host oracle.
// sfm_bounded_schur_test --device N|uuid:<hex> [--validate]
// Refuses automatic selection, software devices, NVIDIA and CPU fallback.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "sfm/ba/Solver.h"
#include "sfm/tests/SyntheticBA.h"
#include "sfm/tests/TestMain.h"

namespace {

int g_fail = 0;
// The established DF rig parity envelope, not a rounding-equivalence claim.
constexpr double kParityTol = 1e-4;

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

void report(const std::string& name, double err, double tol = kParityTol) {
    const bool ok = std::isfinite(err) && err < tol;
    printf("%-44s err %.3e (tol %.0e)  %s\n", name.c_str(), err, tol,
           ok ? "PASS" : "FAIL");
    if (!ok) g_fail++;
}

double relMax(const std::vector<double>& got, const std::vector<double>& ref) {
    if (got.size() != ref.size()) return INFINITY;
    double error = 0, scale = 0;
    for (size_t i = 0; i < ref.size(); i++) {
        if (!std::isfinite(got[i]) || !std::isfinite(ref[i])) return INFINITY;
        error = std::max(error, std::fabs(got[i] - ref[i]));
        scale = std::max(scale, std::fabs(ref[i]));
    }
    return error / std::max(1.0, scale);
}

void finiteParams(const BAProblem& P) {
    for (const auto* values : {&P.poses, &P.exts, &P.intr, &P.points})
        for (double v : *values) require(std::isfinite(v), "nonfinite solved parameter");
}

void compareParams(const std::string& name, const BAProblem& got, const BAProblem& ref,
                   double tol = kParityTol) {
    finiteParams(got);
    finiteParams(ref);
    report(name + " frames", relMax(got.poses, ref.poses), tol);
    if (!ref.exts.empty()) report(name + " extrinsics", relMax(got.exts, ref.exts), tol);
    report(name + " intrinsics", relMax(got.intr, ref.intr), tol);
    report(name + " points", relMax(got.points, ref.points), tol);
}

void compareCost(const std::string& name, double got, double ref) {
    require(std::isfinite(got) && std::isfinite(ref), "nonfinite cost");
    report(name, std::fabs(got - ref) / std::max(1.0, std::fabs(ref)));
}

uint32_t modelId(const char* name) {
    for (uint32_t m = 0; m < (uint32_t)kNumModels; m++)
        if (std::strcmp(kModels[m].name, name) == 0) return m;
    throw std::runtime_error(std::string("missing camera model: ") + name);
}

char tierOf(const BAProblem& P) {
    uint32_t dof = 0;
    for (uint32_t i = 0; i < P.num_images; i++)
        dof = std::max(dof, 6 + P.memberFree(i) + P.groups[P.image_group[i]].n_intr);
    return dof <= 12 ? 'c' : dof <= 14 ? 'm' : dof <= 18 ? 'w' : 'x';
}

void checkShape(const BAProblem& P, char tier) {
    require(P.num_obs > 8192 && P.num_obs % 128 != 0,
            "fixture must cross the 8192-observation boundary with a partial workgroup");
    require(P.groups.size() == 1 && P.groups[0].n_intr > 0 && !exclusiveGroups(P),
            "fixture must refine shared intrinsics on the observation Schur path");
    require(P.groups[0].n_intr == kModels[P.groups[0].model].n_intr,
            "fixture must refine the actual registry parameter count");
    require(tierOf(P) == tier, "fixture does not reach its requested camera DOF tier");
    printf("fixture %s tier=%c images=%u frames=%u observations=%u tail=%u dimension=%u\n",
           kModels[P.groups[0].model].name, tier, P.num_images, P.num_frames,
           P.num_obs, P.num_obs % 128, P.n_dim);
}

SolverOptions options(const std::string& selector, bool validate) {
    SolverOptions o;
    o.real = RealCfg::DF64;
    o.device_selector = selector;
    o.solver = SolverSel::Dense;
    o.cg_fallback = CgFallback::Off;
    o.loss = "huber";
    o.loss_param = 1.5f;
    o.init_damping = 1e-2f;
    o.max_iters = 6;
    o.rtol = 0;
    o.patience = 20;
    o.threads = 1;
    o.verbose = false;
    o.validate = validate;
    return o;
}

void requireGpu(const BundleSolver& solver) {
    require(solver.real() == RealCfg::DF64, "DF was not retained; CPU fallback is forbidden");
    require(std::strcmp(solver.stats().solver, "dense") == 0, "dense solver was not retained");
}

void testParity(const char* model, uint32_t rig, char tier, const SolverOptions& opt) {
    const uint32_t m = modelId(model);
    const BAProblem base = synth::makeProblem(m, rig ? 5 : 9, rig ? 901 : 1001,
                                               1, 0.2, 40 + m + rig, -1, rig, true);
    checkShape(base, tier);
    const std::string name = std::string(model) + (rig ? " rig" : "");
    BAProblem Pg = base, Pc = base;
    SolverOptions cpuOpt = opt;
    cpuOpt.real = RealCfg::CPU;
    cpuOpt.device_selector.clear();
    BundleSolver gpu(Pg, opt);
    gpu.init();
    requireGpu(gpu);
    require(!Pg.use_pair_schur, "fixture selected pair Schur instead of observation Schur");
    bacpu::Solver cpu(Pc, cpuOpt);
    cpu.init();
    const float damping = (float)opt.init_damping;
    gpu.debugAssemble(damping);
    cpu.assembleOnly(damping);
    const auto Sg = gpu.debugPackedS(), Sc = cpu.packedS();
    const auto gg = gpu.debugG(), gc = cpu.gradient();
    const size_t packed = (size_t)base.n_dim * (base.n_dim + 1) / 2;
    require(Sg.size() == packed && Sc.size() == packed &&
            gg.size() == base.n_dim && gc.size() == base.n_dim,
            "assembly readback omitted reduced-system entries");
    report(name + " complete S", relMax(Sg, Sc));
    report(name + " complete g", relMax(gg, gc));

    gpu.solve();
    gpu.downloadParams();
    cpu.solve();
    requireGpu(gpu);
    const SolverStats& gs = gpu.stats();
    const SolverStats& cs = cpu.stats();
    require(gs.iterations > 0 && gs.accepted > 0 && std::isfinite(gs.initial_cost) &&
            std::isfinite(gs.final_cost) && gs.final_cost < gs.initial_cost,
            "GPU solve did not make finite descent");
    require(cs.accepted > 0 && std::isfinite(cs.initial_cost) &&
            std::isfinite(cs.final_cost) && cs.final_cost < cs.initial_cost,
            "CPU oracle did not make finite descent");
    compareCost(name + " initial cost", gs.initial_cost, cs.initial_cost);
    compareCost(name + " final cost", gs.final_cost, cs.final_cost);
    compareCost(name + " downloaded cost", gpu.computeCost(), gs.final_cost);
    compareParams(name, Pg, Pc);
}

SolverStats solveCpu(BAProblem& P, SolverOptions opt) {
    opt.real = RealCfg::CPU;
    opt.device_selector.clear();
    bacpu::Solver solver(P, opt);
    solver.init();
    solver.solve();
    finiteParams(P);
    require(std::isfinite(solver.stats().initial_cost) &&
            std::isfinite(solver.stats().final_cost), "nonfinite CPU retry cost");
    return solver.stats();
}

SolverStats solveGpu(BAProblem& P, const SolverOptions& opt) {
    BundleSolver solver(P, opt);
    solver.init();
    requireGpu(solver);
    require(!P.use_pair_schur, "retry fixture selected pair Schur");
    solver.solve();
    solver.downloadParams();
    finiteParams(P);
    compareCost("retry downloaded cost", solver.computeCost(), solver.stats().final_cost);
    return solver.stats();
}

// Cold CPU steps locate the first accepted lambda on one fixed nonlinear
// problem. Replaying its rejected prefix must equal a cold step there: this
// checks restored parameters and reused Jacobians/Bp without new solver hooks.
void testRetry(SolverOptions opt) {
    BAProblem base = synth::makeProblem(modelId("snavely"), 9, 1001, 1, 0.2, 73);
    checkShape(base, 'c');
    // A badly underestimated log-focal makes the undamped linearization overshoot.
    base.intr[0] -= 4.0;
    opt.loss = "trivial";
    opt.init_damping = 1e-4f;
    opt.max_iters = 1;
    constexpr int kTrials = 10;
    opt.patience = kTrials + 1;
    const double initialDamping = opt.init_damping;
    double damping = initialDamping, rejectMult = 2.0;
    int rejected = 0;
    BAProblem coldCpu;
    SolverStats coldStats;
    for (; rejected < kTrials; rejected++) {
        coldCpu = base;
        opt.init_damping = damping;
        coldStats = solveCpu(coldCpu, opt);
        require(coldStats.iterations == 1, "CPU cold retry did not execute one iteration");
        if (coldStats.accepted == 1) break;
        require(coldStats.accepted == 0, "unexpected CPU cold acceptance count");
        compareParams("CPU rejected restore", coldCpu, base, 1e-12);
        damping *= rejectMult;
        rejectMult = std::min(32.0, rejectMult * 2.0);
    }
    require(rejected > 0 && rejected < kTrials,
            "fixed retry fixture needs a rejected prefix followed by finite acceptance");
    require(coldStats.final_cost < coldStats.initial_cost,
            "cold retry oracle accepted without descent");
    printf("retry oracle rejected=%d first_accepted_lambda=%.9g\n", rejected, damping);

    BAProblem first = base;
    opt.init_damping = initialDamping;
    const SolverStats firstStats = solveGpu(first, opt);
    require(firstStats.iterations == 1 && firstStats.accepted == 0,
            "GPU fixture did not reject its first step");
    compareParams("GPU rejected restore", first, base, 1e-12);
    compareCost("GPU rejected cost", firstStats.final_cost, firstStats.initial_cost);

    BAProblem coldGpu = base;
    opt.init_damping = damping;
    const SolverStats coldGpuStats = solveGpu(coldGpu, opt);
    require(coldGpuStats.iterations == 1 && coldGpuStats.accepted == 1,
            "GPU cold retry did not accept the oracle lambda");
    compareCost("cold retry cost", coldGpuStats.final_cost, coldStats.final_cost);
    compareParams("cold retry", coldGpu, coldCpu);

    BAProblem retryGpu = base, retryCpu = base;
    opt.init_damping = initialDamping;
    opt.max_iters = rejected + 1;
    const SolverStats retryStats = solveGpu(retryGpu, opt);
    const SolverStats retryCpuStats = solveCpu(retryCpu, opt);
    // With rtol=0 and patience beyond the prefix, iterations-accepted counts
    // rejects exactly; every iteration after the initial reject uses reuse.
    require(retryStats.iterations == rejected + 1 && retryStats.accepted == 1 &&
            retryCpuStats.iterations == rejected + 1 && retryCpuStats.accepted == 1,
            "reject/reuse/retry acceptance sequence differs from cold CPU steps");
    compareCost("reuse vs cold GPU cost", retryStats.final_cost, coldGpuStats.final_cost);
    compareCost("reuse vs CPU cost", retryStats.final_cost, retryCpuStats.final_cost);
    compareParams("reuse vs cold GPU", retryGpu, coldGpu);
    compareParams("reuse vs CPU", retryGpu, retryCpu);
    compareParams("CPU reuse vs cold", retryCpu, coldCpu, 1e-10);
    printf("retry iterations=%d accepted=%d rejected=%d\n", retryStats.iterations,
           retryStats.accepted, retryStats.iterations - retryStats.accepted);
}

int run(int argc, char** argv) {
    std::string device;
    bool validate = false;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--device" && i + 1 < argc && device.empty()) device = argv[++i];
        else if (arg == "--validate") validate = true;
        else throw std::runtime_error("usage: sfm_bounded_schur_test --device N|uuid:<hex> [--validate]");
    }
    require(!device.empty(), "--device is required; automatic selection is forbidden");
    auto request = spirula::vkselect::parseRequest(device);
    require(request.kind == spirula::vkselect::Request::Kind::Ordinal ||
            request.kind == spirula::vkselect::Request::Kind::Uuid,
            "--device must be an explicit ordinal or UUID");
    request.explicit_request = true;
    const auto& resolution = VkContext::cachedSelector(request);
    if (!resolution.ok()) throw std::runtime_error(resolution.error);
    const auto& props = resolution.device.props;
    require(props.vendorID != 0x10de, "NVIDIA execution is not supported");
    require(props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
            props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
            "a physical discrete or integrated GPU is required");
    require(realSupportedByDevice(RealCfg::DF64, cachedDeviceCaps(resolution.selector)),
            "selected physical device cannot execute DF; CPU fallback is forbidden");
    printf("strict GPU %s [%s] vendor=0x%x real=df solver=dense\n",
           resolution.device.name.c_str(), resolution.selector.c_str(), props.vendorID);
    const SolverOptions opt = options(resolution.selector, validate);
    testParity("simple_pinhole", 0, 'c', opt);
    testParity("opencv", 0, 'm', opt);
    testParity("full_opencv", 0, 'w', opt);
    testParity("full_opencv", 2, 'x', opt);
    testRetry(opt);
    printf("%s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) { return sfmTestMain(argc, argv, run); }
