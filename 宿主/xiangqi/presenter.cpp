#include "presenter.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace zlong::xiangqi {

namespace {

using engine::Mat4;
using engine::RotationX;
using engine::RotationY;
using engine::Scale;
using engine::Translation;

constexpr float kPi = 3.14159265358979323846f;
/// Pieces stand a hair above the board so nothing z-fights with it.
constexpr float kLift = 0.020f;
/// Figures are drawn a little larger than the square they stand on, so a board of
/// thirty-two reads at a glance rather than at a squint.
constexpr float kFigureScale = 1.0f;
/// How far a struck piece tips before it sinks: a little past horizontal, so it reads as
/// fallen rather than as leaning.
constexpr float kFallAngle = 1.62f;

/// Timings, in seconds. A plain move is a slide; a capture is a lunge or a shot followed
/// by the victim going over. Short enough that play does not wait on them.
constexpr double kSlideTotal = 0.38;
constexpr double kChargeHit = 0.28;
constexpr double kChargeHitEnd = 0.42;
constexpr double kChargeTotal = 0.92;
constexpr double kShotHit = 0.24;
constexpr double kShotHitEnd = 0.44;
constexpr double kShotTotal = 0.94;

float Smooth(double u) {
    const auto f = static_cast<float>(std::clamp(u, 0.0, 1.0));
    return f * f * (3.0f - 2.0f * f);
}

std::string SquareText(Square square) {
    return std::to_string(square.file) + "," + std::to_string(square.rank);
}

/// The two kinds of blow. A cannon shoots; everything else closes and strikes.
bool Shoots(Kind kind) { return kind == Kind::Cannon; }

}  // namespace

Presenter::Presenter(Board board) : board_(std::move(board)) {
    cursor_ = board_.GeneralOf(Side::Red);
    if (!OnBoard(cursor_)) {
        cursor_ = Square{4, 0};
    }
}

std::uint32_t Presenter::Add(engine::Scene& scene, std::uint32_t mesh, std::uint32_t material,
                             const Mat4& local, std::int32_t parent) {
    scene.AddNode(mesh, material, local, parent);
    return static_cast<std::uint32_t>(scene.nodes.size() - 1);
}

void Presenter::Build(engine::Scene& scene) {
    assets_ = AddAssets(scene);

    // The slab gives the board an edge; the surface carries the drawn grid. They are two
    // nodes because the slab has to be plain and the surface has to be textured.
    board_slab_ = Add(scene, assets_.box, assets_.slab_material,
                      Translation(0.0f, -0.29f, 0.0f) *
                          Scale(kSlabWidth + 0.4f, 0.58f, kSlabDepth + 0.4f));
    board_surface_ = Add(scene, assets_.plane, assets_.board_material,
                         Translation(0.0f, 0.006f, 0.0f) * Scale(kSlabWidth, 1.0f, kSlabDepth));

    // The room around it: floor, rug, table, walls. Static, added once, never rewritten.
    for (const FigurePart& part : Room(assets_)) {
        Add(scene, part.mesh, part.material, part.local);
    }

    // One root per piece, with that piece's three primitives under it. The roots carry the
    // animation, the parts never move.
    for (const Piece& piece : board_.pieces()) {
        const std::uint32_t root = Add(scene, engine::kNoMesh, 0, engine::Identity());
        piece_node_.push_back(root);
        for (const FigurePart& part : Figure(piece.kind, piece.side == Side::Red, assets_)) {
            Add(scene, part.mesh, part.material, part.local, static_cast<std::int32_t>(root));
        }
    }

    cursor_node_ = Add(scene, assets_.cylinder, assets_.cursor,
                       Translation(0.0f, 0.010f, 0.0f) * Scale(1.0f, 0.020f, 1.0f));
    // A ring under whichever general is in check, so being in check is visible and not
    // only audible from the console.
    check_node_ = Add(scene, assets_.cylinder, assets_.capture_marker,
                      Translation(0.0f, 0.006f, 0.0f) * Scale(1.0f, 0.012f, 1.0f));
    // The move just played, marked where it came from and where it went. Without this the
    // other side's move happens and there is nothing left to say which piece it was.
    trail_from_node_ = Add(scene, assets_.cylinder, assets_.trail,
                           Translation(0.0f, 0.004f, 0.0f) * Scale(0.62f, 0.008f, 0.62f));
    trail_to_node_ = Add(scene, assets_.cylinder, assets_.trail,
                         Translation(0.0f, 0.006f, 0.0f) * Scale(0.44f, 0.012f, 0.44f));
    for (int index = 0; index < kMarkerCount; ++index) {
        marker_node_.push_back(Add(scene, assets_.cylinder, assets_.move_marker,
                                   Translation(0.0f, 0.006f, 0.0f) * Scale(0.34f, 0.012f, 0.34f)));
    }
    shot_node_ = Add(scene, assets_.sphere, assets_.projectile, Parked());
}

engine::Vec3 Presenter::CentreOf(Square square) const {
    engine::Vec3 centre = SquareCentre(square);
    centre.y = kLift;
    return centre;
}

Mat4 Presenter::Parked() const { return Translation(0.0f, -60.0f, 0.0f); }

Mat4 Presenter::Placement(const Piece& piece) const {
    const engine::Vec3 centre = CentreOf(piece.at);
    const float facing = piece.side == Side::Black ? kPi : 0.0f;
    return Translation(centre.x, centre.y, centre.z) * RotationY(facing) * Scale(kFigureScale);
}

Mat4 Presenter::MoverTransform(const Piece& piece) const {
    const engine::Vec3 a = CentreOf(animation_.from);
    const engine::Vec3 b = CentreOf(animation_.to);
    const float facing = piece.side == Side::Black ? kPi : 0.0f;

    double travel = 0.0;
    float hop = 0.0f;
    switch (animation_.kind) {
        case Animation::Kind::Slide:
            travel = Smooth(animation_.t / kSlideTotal);
            break;
        case Animation::Kind::Charge:
            // Most of the way at a walk, then the last stretch fast: that is the blow.
            if (animation_.t < animation_.hit_start) {
                travel = 0.72 * Smooth(animation_.t / animation_.hit_start);
            } else {
                travel = 0.72 + 0.28 * Smooth((animation_.t - animation_.hit_start) /
                                              (animation_.hit_end - animation_.hit_start));
            }
            break;
        case Animation::Kind::Shoot:
            // A cannon stays where it is while the shot flies, then hops the screen it
            // fired over and lands on the square.
            if (animation_.t <= animation_.hit_end) {
                travel = 0.0;
            } else {
                const double u =
                    (animation_.t - animation_.hit_end) / (animation_.total - animation_.hit_end);
                travel = Smooth(u);
                hop = 0.5f * std::sin(kPi * static_cast<float>(std::clamp(u, 0.0, 1.0)));
            }
            break;
        case Animation::Kind::None:
            break;
    }

    const auto f = static_cast<float>(travel);
    const engine::Vec3 at{a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f + hop,
                          a.z + (b.z - a.z) * f};
    return Translation(at.x, at.y, at.z) * RotationY(facing) * Scale(kFigureScale);
}

Mat4 Presenter::VictimTransform(const Piece& piece) const {
    const engine::Vec3 base = CentreOf(animation_.victim_at);
    const engine::Vec3 attacker = CentreOf(animation_.to);
    const float facing = piece.side == Side::Black ? kPi : 0.0f;

    const double fall = animation_.total - animation_.hit_end;
    const double u = fall <= 0.0
                         ? 1.0
                         : std::clamp((animation_.t - animation_.hit_end) / fall, 0.0, 1.0);
    const float eased = Smooth(u);

    // Struck away from whoever hit it, so the blow has a direction.
    engine::Vec3 away{base.x - attacker.x, 0.0f, base.z - attacker.z};
    if (away.x == 0.0f && away.z == 0.0f) {
        away.z = 1.0f;
    }
    const float length = std::sqrt(away.x * away.x + away.z * away.z);
    away.x /= length;
    away.z /= length;
    const float yaw = std::atan2(away.x, away.z);

    const float recoil = 0.20f * eased;
    float sink = 0.0f;
    if (u > 0.55) {
        const auto k = static_cast<float>((u - 0.55) / 0.45);
        sink = -1.7f * k * k;
    }
    return Translation(base.x + away.x * recoil, base.y + sink, base.z + away.z * recoil) *
           RotationY(yaw) * RotationX(kFallAngle * eased) * RotationY(facing) *
           Scale(kFigureScale);
}

void Presenter::HandleInput(const Keys& keys) {
    if (keys.restart > 0) {
        board_ = Board::Start();
        cursor_ = Square{4, 0};
        selected_ = -1;
        selected_moves_.clear();
        over_ = false;
        event_ = "a new game";
        return;
    }
    // One press, one step: a frame can hold several presses and they all happened.
    //
    // Left walks the *file up*, which reads backwards until you remember where the camera is:
    // it stands behind red looking along +Z, so its right hand points along -X, and a larger
    // file is further towards -X. On screen, then, file 0 is the right-hand column and file 8
    // the left; the arrow that goes left has to be the one that counts up.
    for (int step = 0; step < keys.left; ++step) {
        cursor_.file = std::min(kFiles - 1, cursor_.file + 1);
    }
    for (int step = 0; step < keys.right; ++step) {
        cursor_.file = std::max(0, cursor_.file - 1);
    }
    for (int step = 0; step < keys.up; ++step) {
        cursor_.rank = std::min(kRanks - 1, cursor_.rank + 1);
    }
    for (int step = 0; step < keys.down; ++step) {
        cursor_.rank = std::max(0, cursor_.rank - 1);
    }
    if (keys.cancel > 0) {
        selected_ = -1;
        selected_moves_.clear();
    }
    if (keys.confirm > 0) {
        Confirm();
    }
}

void Presenter::Confirm() {
    const Piece* on = board_.At(cursor_);
    if (selected_ >= 0) {
        // A destination first: that is what the markers are for.
        for (const Move& move : selected_moves_) {
            if (move.to == cursor_) {
                BeginMove(move);
                return;
            }
        }
    }
    // Otherwise this selects a piece, or switches to another one.
    if (on != nullptr && on->side == board_.to_move()) {
        const auto moves = board_.MovesFrom(cursor_);
        if (!moves.empty()) {
            selected_ = board_.IndexAt(cursor_);
            selected_moves_ = moves;
            return;
        }
    }
    selected_ = -1;
    selected_moves_.clear();
}

void Presenter::BeginMove(const Move& move) {
    const int mover = board_.IndexAt(move.from);
    const int victim = board_.IndexAt(move.to);
    if (mover < 0) {
        return;
    }
    const Kind kind = board_.pieces()[static_cast<std::size_t>(mover)].kind;
    const Side side = board_.pieces()[static_cast<std::size_t>(mover)].side;
    const std::string taken =
        victim >= 0 ? std::string(" takes ") +
                          Name(board_.pieces()[static_cast<std::size_t>(victim)].side) + " " +
                          Name(board_.pieces()[static_cast<std::size_t>(victim)].kind)
                    : std::string();

    // The rules move first. Everything the frames below do is a picture of a move that has
    // already happened.
    if (!board_.Apply(move.from, move.to)) {
        return;
    }
    event_ = std::string(Name(side)) + " " + Name(kind) + " " + SquareText(move.from) + " -> " +
             SquareText(move.to) + taken;

    animation_ = Animation{};
    animation_.mover = mover;
    animation_.victim = victim;
    animation_.from = move.from;
    animation_.to = move.to;
    animation_.victim_at = move.to;
    animation_.t = 0.0;
    if (victim < 0) {
        animation_.kind = Animation::Kind::Slide;
        animation_.hit_start = 0.0;
        animation_.hit_end = 0.0;
        animation_.total = kSlideTotal;
    } else if (Shoots(kind)) {
        animation_.kind = Animation::Kind::Shoot;
        animation_.hit_start = kShotHit;
        animation_.hit_end = kShotHitEnd;
        animation_.total = kShotTotal;
    } else {
        animation_.kind = Animation::Kind::Charge;
        animation_.hit_start = kChargeHit;
        animation_.hit_end = kChargeHitEnd;
        animation_.total = kChargeTotal;
    }
    selected_ = -1;
    selected_moves_.clear();
}

void Presenter::Finish() {
    animation_ = Animation{};
    if (over_) {
        return;  // the beaten general's topple, finishing
    }
    const Result result = board_.result();
    if (result.outcome == Outcome::Playing) {
        return;
    }
    over_ = true;
    if (result.outcome == Outcome::Draw) {
        event_ = std::string("draw: ") + Name(result.why);
        return;  // nobody was beaten, so nothing goes over
    }

    const Side winner = result.outcome == Outcome::RedWins ? Side::Red : Side::Black;
    event_ = std::string(Name(winner)) + " wins by " + Name(result.why);

    // The beaten general goes over, the way a struck piece does. The console says the game is
    // lost; this is the board saying it too. It goes over by 长将 as readily as by mate.
    const Square square = board_.GeneralOf(Opponent(winner));
    const int general = board_.IndexAt(square);
    if (general < 0) {
        return;
    }
    animation_.kind = Animation::Kind::Charge;
    animation_.mover = -1;
    animation_.victim = general;
    animation_.from = square;
    animation_.to = square;
    animation_.victim_at = square;
    animation_.hit_start = 0.0;
    animation_.hit_end = 0.12;
    animation_.total = 0.12 + 0.7;
}

void Presenter::PlayComputer() {
    const auto move = computer_.Choose(board_);
    if (!move.has_value()) {
        return;
    }
    const double think = computer_.last_think_ms();
    BeginMove(*move);
    // How long it thought goes on the console line, because "the computer takes a moment"
    // is a claim and this is the measurement.
    if (!event_.empty()) {
        event_ += "  [computer " + std::to_string(static_cast<int>(think + 0.5)) + " ms]";
    }
}

void Presenter::Update(engine::Scene& scene, const Keys& keys, double seconds) {
    if (keys.toggle_computer > 0) {
        computer_black_ = !computer_black_;
        event_ = computer_black_ ? "the computer plays black" : "both sides are yours";
    }
    if (animation_.kind != Animation::Kind::None) {
        animation_.t += seconds;
        if (animation_.t >= animation_.total) {
            Finish();
        }
    } else if (!board_.Finished()) {
        const bool computer_turn =
            self_play_ || (computer_black_ && board_.to_move() == Side::Black);
        if (computer_turn) {
            PlayComputer();
        } else {
            HandleInput(keys);
        }
    } else if (keys.restart) {
        HandleInput(keys);
    }
    Write(scene);
}

void Presenter::Write(engine::Scene& scene) {
    const bool busy = animation_.kind != Animation::Kind::None;

    for (std::size_t index = 0; index < board_.pieces().size(); ++index) {
        const Piece& piece = board_.pieces()[index];
        const auto as_int = static_cast<int>(index);
        Mat4 world;
        bool visible = piece.alive;
        if (busy && animation_.victim == as_int) {
            // Struck pieces are on screen even though the board already says they are gone.
            world = VictimTransform(piece);
            visible = true;
        } else if (busy && animation_.mover == as_int) {
            world = MoverTransform(piece);
        } else if (piece.alive) {
            world = Placement(piece);
        }
        if (!visible) {
            world = Parked();
        }
        scene.nodes[piece_node_[index]].local = world;
    }

    if (!busy && !board_.Finished()) {
        const engine::Vec3 centre = CentreOf(cursor_);
        scene.nodes[cursor_node_].local = Translation(centre.x, 0.010f, centre.z) *
                                          Scale(1.0f, 0.020f, 1.0f);
    } else {
        scene.nodes[cursor_node_].local = Parked();
    }

    if (!busy && !over_ && board_.InCheck(board_.to_move())) {
        const Square square = board_.GeneralOf(board_.to_move());
        if (OnBoard(square)) {
            const engine::Vec3 centre = CentreOf(square);
            scene.nodes[check_node_].local =
                Translation(centre.x, 0.006f, centre.z) * Scale(1.0f, 0.012f, 1.0f);
        }
    } else {
        scene.nodes[check_node_].local = Parked();
    }

    if (board_.last_move().valid) {
        const engine::Vec3 from = CentreOf(board_.last_move().from);
        const engine::Vec3 to = CentreOf(board_.last_move().to);
        scene.nodes[trail_from_node_].local =
            Translation(from.x, 0.004f, from.z) * Scale(0.62f, 0.008f, 0.62f);
        scene.nodes[trail_to_node_].local =
            Translation(to.x, 0.006f, to.z) * Scale(0.44f, 0.012f, 0.44f);
    } else {
        scene.nodes[trail_from_node_].local = Parked();
        scene.nodes[trail_to_node_].local = Parked();
    }

    for (int index = 0; index < kMarkerCount; ++index) {
        const auto node = marker_node_[static_cast<std::size_t>(index)];
        const bool needed = !busy && selected_ >= 0 &&
                            index < static_cast<int>(selected_moves_.size());
        if (!needed) {
            scene.nodes[node].local = Parked();
            continue;
        }
        const Move& move = selected_moves_[static_cast<std::size_t>(index)];
        const engine::Vec3 centre = CentreOf(move.to);
        scene.nodes[node].local =
            Translation(centre.x, 0.006f, centre.z) * Scale(0.34f, 0.012f, 0.34f);
        // A square that would be a capture is marked in the colour of one.
        scene.nodes[node].material = move.capture ? assets_.capture_marker : assets_.move_marker;
    }

    // The shot, only while it is in the air. It leaves the cannon's muzzle on its old
    // square and lands on the piece it was fired at.
    if (busy && animation_.kind == Animation::Kind::Shoot && animation_.t >= animation_.hit_start &&
        animation_.t <= animation_.hit_end) {
        const engine::Vec3 cannon = CentreOf(animation_.from);
        const engine::Vec3 target = CentreOf(animation_.victim_at);
        const float muzzle_y = cannon.y + 0.72f;
        const float aim_y = target.y + 0.62f;
        const auto u = static_cast<float>(
            std::clamp((animation_.t - animation_.hit_start) /
                           (animation_.hit_end - animation_.hit_start),
                       0.0, 1.0));
        const auto x = cannon.x + (target.x - cannon.x) * u;
        const auto y = muzzle_y + (aim_y - muzzle_y) * u + 0.28f * std::sin(kPi * u);
        const auto z = cannon.z + (target.z - cannon.z) * u;
        scene.nodes[shot_node_].local = Translation(x, y, z) * Scale(0.16f);
    } else {
        scene.nodes[shot_node_].local = Parked();
    }
}

std::string Presenter::TakeEvent() {
    std::string taken;
    taken.swap(event_);
    return taken;
}

}  // namespace zlong::xiangqi
