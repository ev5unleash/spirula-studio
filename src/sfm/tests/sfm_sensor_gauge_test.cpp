// The sensor gauge on a synthetic capture: a walking trajectory with a
// known IMU-to-lens rotation, IMU noise, biases, a GPS log, a clock offset
// and a random model gauge, recovered to a fraction of a degree and a
// percent. Prints FAIL lines and returns the count.
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "sfm/core/Model.h"
#include "sfm/core/SensorTimeline.h"
#include "sfm/core/Telemetry.h"
#include "sfm/map/Orient.h"
#include "sfm/map/SensorGauge.h"
#include "sfm/tests/TestMain.h"

using namespace sfm;

static int fails = 0;
static void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what.c_str());
        fails++;
    }
}

static double angleDeg(const Mat3& A, const Mat3& B) {
    const Mat3 D = mul(transpose(A), B);
    const double tr = std::max(-1.0, std::min(1.0, (D[0] + D[4] + D[8] - 1.0) * 0.5));
    return std::acos(tr) * 180.0 / M_PI;
}
static double angleDeg(const Vec3& a, const Vec3& b) {
    const double d = std::max(-1.0, std::min(1.0, a.dot(b) / (a.norm() * b.norm())));
    return std::acos(d) * 180.0 / M_PI;
}

// ---- the synthetic world ---------------------------------------------------

struct Scenario {
    double duration = 120;
    double motion = 1.0;       // 0 = a camera that never moves
    double turning = 1.0;      // 0 = a camera that never rotates
    bool gps = true;
    bool stale_gps = false;
    bool mirrored = false;     // IMU axes reported left-handed
    double clock_offset = 0;   // IMU clock ahead of the video by this many seconds
    Vec3 bg{0.004, -0.003, 0.002};
    Vec3 ba{0.03, -0.02, 0.05};
    double gyro_noise = 0.002, accel_noise = 0.02;
    // A camera writing a fused attitude and one accelerometer reading per
    // frame instead of a raw gyro: the DJI Osmo 360.
    bool attitude_only = false;
    double accel_rate = 0;   // 0 keeps the 1 kHz the gyro is written at
    unsigned seed = 7;
};

// Camera-to-world pose of the walk at t: an arc with a bob and a wobble.
static void poseAt(const Scenario& sc, double t, Mat3& R_wc, Vec3& p) {
    const double m = sc.motion, q = sc.turning;
    const double w = 2 * M_PI / 60;
    // A walk round a circle with speed changes and turns of about 0.5 m/s^2:
    // the low-frequency accelerations frames a second apart can see.
    const double th = (w * t + 0.12 * std::sin(0.5 * t) + 0.03 * std::sin(1.3 * t + 0.7)) * m;
    const double rad = 15 + 0.6 * std::sin(0.9 * t) * m;
    p = {rad * std::cos(th) + 0.03 * std::sin(2 * M_PI * 2 * t) * m,
         rad * std::sin(th) + 0.03 * std::cos(2 * M_PI * 1.9 * t) * m,
         1.5 + 0.02 * std::sin(2 * M_PI * 2 * t + 0.4) * m + 0.4 * std::sin(0.31 * t) * m};
    const double yaw = (th + 0.5 * std::sin(0.7 * t)) * q, pitch = 0.35 * std::sin(0.45 * t + 1.0) * q,
                 roll = 0.25 * std::sin(0.83 * t + 2.0) * q;
    // Camera looks along +z of a frame that is yawed/pitched/rolled; y down.
    const Mat3 Rz = angleAxisToRotation({0, 0, yaw});
    const Mat3 Rx = angleAxisToRotation({pitch, 0, 0});
    const Mat3 Ry = angleAxisToRotation({0, roll, 0});
    // Base: camera z = world x (forward), camera y = world -z (down), camera x = world -y.
    const Mat3 base = {0, -1, 0, 0, 0, -1, 1, 0, 0};   // columns are camera axes in world
    R_wc = mul(mul(mul(Rz, Rx), Ry), transpose(base));
}

static Telemetry synthesize(const Scenario& sc, const Mat3& R_ci) {
    std::mt19937 rng(sc.seed);
    std::normal_distribution<double> N(0, 1);
    Telemetry t;
    t.carrier = TelemetryCarrier::Camm;
    t.camera = "synthetic";
    t.video_fps = 24;
    t.video_duration = sc.duration;
    const double dt = 1e-3;
    const Vec3 g_w{0, 0, -9.81};
    const Mat3 D = sc.mirrored ? Mat3{-1, 0, 0, 0, 1, 0, 0, 0, 1} : mat3Identity();
    for (double ti = -0.5; ti <= sc.duration + 0.5; ti += dt) {
        Mat3 R0, R1, R2;
        Vec3 p0, p1, p2;
        const double h = 2e-3;
        poseAt(sc, ti - h, R0, p0);
        poseAt(sc, ti, R1, p1);
        poseAt(sc, ti + h, R2, p2);
        const Vec3 acc_w = (p2 - p1 * 2.0 + p0) * (1.0 / (h * h));
        // omega in the camera frame from the finite-difference of R_wc.
        const Mat3 dR = mul(transpose(R0), R2);
        const Vec3 omega_c = rotationToAngleAxis(dR) * (1.0 / (2 * h));
        const Mat3 R_wi = mul(R1, R_ci);
        const Vec3 omega_i = mul(transpose(R_ci), omega_c);
        const Vec3 f_i = mul(transpose(R_wi), acc_w - g_w);
        const double adt = sc.accel_rate > 0 ? 1.0 / sc.accel_rate : dt;
        Vec3 wm = mul(D, omega_i + sc.bg + Vec3{N(rng), N(rng), N(rng)} * (sc.gyro_noise / std::sqrt(dt)));
        Vec3 am = mul(D, f_i + sc.ba + Vec3{N(rng), N(rng), N(rng)} * (sc.accel_noise / std::sqrt(adt)));
        const double tv = ti + sc.clock_offset;
        if (sc.attitude_only) {
            const Quat q = rotationToQuaternion(mul(R_wi, transpose(D)));
            t.orientation.push_back({tv, q[0], q[1], q[2], q[3]});
        } else {
            t.gyro.push_back({tv, wm.x, wm.y, wm.z});
        }
        if (sc.accel_rate > 0 && std::fmod(ti + 0.5, adt) >= dt) continue;
        t.accel.push_back({tv, am.x, am.y, am.z});
    }
    if (sc.gps) {
        const double lat0 = 43.66, lon0 = -79.39;
        const double Re = 6378137.0;
        for (double ti = 0; ti <= sc.duration; ti += 0.1) {
            Mat3 R;
            Vec3 p;
            const double tf = sc.stale_gps ? 0.0 : std::floor(ti);   // 1 Hz updates
            poseAt(sc, tf, R, p);
            std::mt19937 rj((unsigned)(tf * 1000) + sc.seed);
            const double e = p.x + 1.0 * N(rj), n = p.y + 1.0 * N(rj), u = p.z + 3.0 * N(rj);
            TelemetryGps g;
            g.t = ti;
            g.fix = true;
            g.lat = lat0 + n / Re * 180 / M_PI;
            g.lon = lon0 + e / (Re * std::cos(lat0 * M_PI / 180)) * 180 / M_PI;
            g.alt = 100 + u;
            g.has_alt = true;
            g.dop = 1.5;
            t.gps.push_back(g);
        }
    }
    return t;
}

// The reconstruction: one image per second, poses in a random gauge M
// (world = M(model)), so the solver has to find M.
static Reconstruction synthesizeModel(const Scenario& sc, const Sim3& M, double fps_frames = 1.0) {
    Reconstruction rec;
    Camera cam;
    cam.width = 1000;
    cam.height = 1000;
    rec.cameras[1] = cam;
    const Sim3 Minv = invertSim3(M);
    uint32_t id = 1;
    for (double ti = 0.5; ti < sc.duration; ti += 1.0 / fps_frames) {
        Mat3 R_wc;
        Vec3 p;
        poseAt(sc, ti, R_wc, p);
        Pose world;
        world.R = transpose(R_wc);
        world.t = mul(world.R, p) * -1.0;
        Image im;
        im.id = id;
        im.camera_id = 1;
        char name[64];
        std::snprintf(name, sizeof name, "cam0/%05d.jpg", (int)std::lround(ti * 24));
        im.name = name;
        im.pose = transformPose(Minv, world);
        im.registered = true;
        rec.images[id++] = im;
    }
    return rec;
}

static void printGroups(const SensorGaugeResult& r) {
    for (const TimeOffsetFit& t : r.time_offsets)
        std::printf("   offset: found=%d %.1f ms gain=%.2f pairs=%d\n", t.found, 1000 * t.offset, t.gain, t.pairs);
    for (const SensorGroupReport& g : r.groups)
        std::printf("   group %s: frames=%d triples=%d inl=%d s=%.4f sig=%.3f g=%.2f ang=%.1f "
                    "flipped=%d calib ok=%d reason=%d rms=%.2f gap=%.1f rot=%d grav=%d sig_rot=%.2f "
                    "sig_grav=%.2f | up ok=%d votes=%d spread=%.2f outliers=%d\n",
                    g.name.c_str(), g.frames, g.triples, g.triple_inliers, g.scale, g.scale_sigma,
                    g.g_norm, g.g_angle_deg, g.flipped, g.fit.ok, (int)g.fit.reason, g.fit.rms, g.fit.gap,
                    g.fit.rot_pairs, g.fit.grav_pairs, g.fit.sig_rot_deg, g.fit.sig_grav_deg, r.up.ok,
                    r.up.votes, r.up.spread_deg, r.up.outliers);
}

struct RunResult {
    SensorGaugeResult r;
    Sim3 M;
    Mat3 R_ci;
    double up_err_deg = 0, scale_err = 0, calib_err_deg = 0;
};

static RunResult runScenario(const Scenario& sc, SensorMode mode = SensorMode::Auto) {
    RunResult out;
    out.R_ci = angleAxisToRotation(Vec3{0.3, -1.2, 0.7});
    Telemetry t = synthesize(sc, out.R_ci);
    const TelemetryCheck c = telemetry_check(t);
    SensorTimeline tl;
    std::string err;
    check(tl.init(t, c, err), "timeline init: " + err);
    Sim3 M;
    M.scale = 0.37;
    M.R = angleAxisToRotation(Vec3{1.1, 0.4, -0.9});
    M.t = {2.0, -1.0, 0.5};
    out.M = M;
    Reconstruction rec = synthesizeModel(sc, M);
    SensorCapture cap;
    cap.prefix = "";
    cap.fps = 24;
    cap.timeline = &tl;
    SensorGaugeOptions opt;
    opt.mode = mode;
    out.r = fitSensorGauge(rec, {cap}, opt);
    const SensorGaugeResult& r = out.r;
    // The recovered T must take model up to +Z and model units to metres.
    const Vec3 up_model = mul(transpose(M.R), Vec3{0, 0, 1});
    out.up_err_deg = angleDeg(mul(r.T.R, up_model), Vec3{0, 0, 1});
    out.scale_err = r.T.scale / M.scale - 1.0;
    if (!r.groups.empty()) {
        const Mat3 Dm = sc.mirrored ? Mat3{-1, 0, 0, 0, 1, 0, 0, 0, 1} : mat3Identity();
        out.calib_err_deg = angleDeg(mul(r.groups[0].fit.R_ci, Dm), out.R_ci);
    }
    return out;
}

int cmdSensorGaugeTest(int, char**) {
    // ---- T1: IMU + GPS, the ordinary case -----------------------------------
    {
        Scenario sc;
        RunResult rr = runScenario(sc);
        const SensorGaugeResult& r = rr.r;
        std::printf("T1 imu+gps: applied=%d metric=%d up=%.3f deg scale_err=%.4f calib=%.3f deg "
                    "s_imu=%.4f(%.2f%%) s_gps=%.4f(%.2f%%) offset=%.1f ms lm=%d\n",
                    r.applied, r.metric, rr.up_err_deg, rr.scale_err, rr.calib_err_deg,
                    r.scale_imu, 100 * r.scale_imu_sigma, r.scale_gps, 100 * r.scale_gps_sigma,
                    1000 * r.time_offsets[0].offset, r.lm_iterations);
        printGroups(r);
        check(r.applied && r.metric, "T1 applied metric");
        check(r.up_from_imu && rr.up_err_deg < 0.5, "T1 up within 0.5 deg");
        check(std::fabs(rr.scale_err) < 0.01, "T1 scale within 1%");
        check(rr.calib_err_deg < 1.0, "T1 extrinsic within 1 deg");
        check(r.scale_from_imu && r.scale_from_gps, "T1 both scale sources");
        check(!r.groups[0].fit.mirrored, "T1 right-handed");
        check(std::fabs(r.time_offsets[0].offset) < 0.005, "T1 no spurious clock offset");
    }
    // ---- T2: IMU only, indoor -------------------------------------------------
    {
        Scenario sc;
        sc.gps = false;
        RunResult rr = runScenario(sc);
        const SensorGaugeResult& r = rr.r;
        std::printf("T2 imu only: up=%.3f deg scale_err=%.4f sigma=%.2f%% g=%.2f m/s^2 %.2f deg\n",
                    rr.up_err_deg, rr.scale_err, 100 * r.scale_imu_sigma, r.groups[0].g_norm,
                    r.groups[0].g_angle_deg);
        check(r.applied && r.metric && r.scale_from_imu && !r.scale_from_gps, "T2 metric from IMU");
        check(std::fabs(rr.scale_err) < 0.02, "T2 scale within 2%");
        check(std::fabs(r.groups[0].g_norm - 9.81) < 0.3, "T2 gravity norm");
    }
    // ---- T3: stale GPS, mirrored IMU axes, a clock offset ----------------------
    {
        Scenario sc;
        sc.stale_gps = true;
        sc.mirrored = true;
        sc.clock_offset = 0.037;
        RunResult rr = runScenario(sc);
        const SensorGaugeResult& r = rr.r;
        std::printf("T3 stale gps, mirrored, offset: up=%.3f scale_err=%.4f calib=%.3f mirrored=%d "
                    "sign=%.0f offset=%.1f ms gps_ok=%d\n",
                    rr.up_err_deg, rr.scale_err, rr.calib_err_deg, r.groups[0].fit.mirrored,
                    r.groups[0].fit.gyro_sign, 1000 * r.time_offsets[0].offset, r.gps.ok);
        check(r.applied && r.metric && r.scale_from_imu, "T3 metric from IMU");
        check(!r.scale_from_gps && r.gps_frames == 0, "T3 stale GPS refused");
        check(r.groups[0].fit.mirrored, "T3 mirror detected");
        check(rr.calib_err_deg < 1.0, "T3 extrinsic within 1 deg");
        check(std::fabs(r.time_offsets[0].offset - 0.037) < 0.004, "T3 clock offset recovered");
        check(std::fabs(rr.scale_err) < 0.02, "T3 scale within 2%");
    }
    // ---- T4: a camera that turns on the spot: up, no scale --------------------
    {
        Scenario sc;
        sc.motion = 0;
        sc.gps = false;
        RunResult rr = runScenario(sc);
        const SensorGaugeResult& r = rr.r;
        std::printf("T4 pan only: applied=%d metric=%d up=%.3f deg imu_sigma=%.1f%% fail=%d\n",
                    r.applied, r.metric, rr.up_err_deg, 100 * r.scale_imu_sigma, (int)r.fail);
        printGroups(r);
        check(r.applied && !r.metric, "T4 upright, not metric");
        check(rr.up_err_deg < 1.0, "T4 up within 1 deg without translation");
        check(r.no_scale == SensorNoScale::ImuWeak, "T4 scale refused as weak");
    }
    // ---- T4b: a camera that never moves at all: nothing to say --------------
    {
        Scenario sc;
        sc.motion = 0;
        sc.turning = 0;
        sc.gps = false;
        RunResult rr = runScenario(sc);
        std::printf("T4b static: applied=%d fail=%d calib=%d\n", rr.r.applied, (int)rr.r.fail,
                    (int)rr.r.groups[0].fit.reason);
        printGroups(rr.r);
        check(!rr.r.applied, "T4b declined");
    }
    // ---- T5: up only mode ------------------------------------------------------
    {
        Scenario sc;
        RunResult rr = runScenario(sc, SensorMode::Up);
        check(rr.r.applied && !rr.r.metric && rr.up_err_deg < 0.5, "T5 up-only mode");
    }
    // ---- T6: GPS only (no IMU streams) ----------------------------------------
    {
        Scenario sc;
        RunResult rr;
        Telemetry t = synthesize(sc, mat3Identity());
        t.gyro.clear();
        t.accel.clear();
        const TelemetryCheck c = telemetry_check(t);
        SensorTimeline tl;
        std::string err;
        check(tl.init(t, c, err), "T6 timeline init: " + err);
        Sim3 M;
        M.scale = 2.2;
        M.R = angleAxisToRotation(Vec3{0.0, 0.0, 0.7});
        Reconstruction rec = synthesizeModel(sc, M);
        SensorCapture cap;
        cap.fps = 24;
        cap.timeline = &tl;
        SensorGaugeOptions opt;
        const SensorGaugeResult r = fitSensorGauge(rec, {cap}, opt);
        std::printf("T6 gps only: applied=%d metric=%d scale=%.4f (true %.4f) frames=%d inl=%d\n",
                    r.applied, r.metric, r.T.scale, M.scale, r.gps_frames, r.gps.inliers);
        check(r.applied && r.metric && r.scale_from_gps && !r.up_from_imu, "T6 metric from GPS");
        check(std::fabs(r.T.scale / M.scale - 1.0) < 0.03, "T6 scale within 3%");
    }
    // ---- T7: a fused attitude and one accelerometer reading per frame ---------
    {
        Scenario sc;
        sc.gps = false;
        sc.attitude_only = true;
        sc.accel_rate = 30;
        sc.accel_noise = 0.06;   // what the DJI's own stream measures at 30 Hz
        RunResult rr = runScenario(sc);
        const SensorGaugeResult& r = rr.r;
        std::printf("T7 attitude, 30 Hz accel: up=%.3f deg scale_err=%.4f sigma=%.2f%% "
                    "triples=%d g=%.2f m/s^2 %.2f deg\n",
                    rr.up_err_deg, rr.scale_err, 100 * r.scale_imu_sigma, r.groups[0].triples,
                    r.groups[0].g_norm, r.groups[0].g_angle_deg);
        printGroups(r);
        check(r.applied && r.metric && r.scale_from_imu, "T7 metric with no gyro");
        check(std::fabs(rr.scale_err) < 0.03, "T7 scale within 3%");
        check(rr.up_err_deg < 1.0, "T7 up within 1 deg");
        check(rr.calib_err_deg < 1.0, "T7 extrinsic within 1 deg");
        check(std::fabs(r.groups[0].g_norm - 9.81) < 0.3, "T7 gravity norm");
    }
    std::printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}

int main(int argc, char** argv) { return sfmTestMain(argc, argv, cmdSensorGaugeTest); }
