// 烛龙 (ZhuLong) - the board on screen, and a key turned into a move.
//
// The other half of the game: `rules.cpp` knows what is legal, this knows what it looks
// like. The renderer locks its node count at Prepare, so the entire fixed pool -- the
// board, all thirty-two pieces, the cursor, the destination markers, the shot -- is built
// once in Build(). Nothing is ever added or removed after that: a captured piece is moved
// out of sight, and a marker that is not needed is parked below the floor.
//
// A capture is animated from the *rules'* record of it, not by diffing the position: the
// move is applied first and the frames that follow are the picture of what already
// happened. That is why the mover's node can be mid-flight while the board says it has
// arrived.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "computer.h"
#include "figures.h"
#include "rules.h"
#include "zlong/engine/scene.h"

namespace zlong::xiangqi {

/// What the keyboard is asking for this frame.
///
/// Counted rather than flagged, because a frame is fifty milliseconds and a key press is not:
/// two taps of an arrow key inside one frame are two moves and have to stay two. The actions
/// are read as "at least once", since confirming twice in one frame is not a thing a person
/// means.
struct Keys {
    int left = 0;
    int right = 0;
    int up = 0;
    int down = 0;
    int confirm = 0;
    int cancel = 0;
    int restart = 0;
    /// Give black to the computer, or take it back.
    int toggle_computer = 0;
};

class Presenter {
public:
    explicit Presenter(Board board);

    /// Append every node the game will ever use, and set the board down. Call before the
    /// machine prepares the scene.
    void Build(engine::Scene& scene);

    /// One frame: take the keys, advance anything in flight, and write the world.
    void Update(engine::Scene& scene, const Keys& keys, double seconds);

    const Board& board() const noexcept { return board_; }

    /// Whether the computer is playing black.
    void set_computer(bool on) noexcept { computer_black_ = on; }
    bool computer_plays() const noexcept { return computer_black_; }
    /// Play both sides, which is how a game gets played through without a person.
    void set_self_play(bool on) noexcept { self_play_ = on; }

    /// The line to print for what just happened, or empty. Cleared once taken.
    std::string TakeEvent();

private:
    /// How many destination markers the pool holds. A chariot on an open line has at most
    /// seventeen moves, so this is more than enough and is a fixed number because the node
    /// count cannot change later.
    static constexpr int kMarkerCount = 20;

    struct Animation {
        enum class Kind { None, Slide, Charge, Shoot } kind = Kind::None;
        double t = 0.0;
        /// When the blow lands, and when it is over. Between them a charge lunges and a
        /// cannon's shot flies.
        double hit_start = 0.0;
        double hit_end = 0.0;
        double total = 0.0;
        int mover = -1;
        int victim = -1;
        Square from;
        Square to;
        Square victim_at;
    };

    static std::uint32_t Add(engine::Scene& scene, std::uint32_t mesh, std::uint32_t material,
                             const engine::Mat4& local, std::int32_t parent = engine::kNoParent);

    void HandleInput(const Keys& keys);
    void Confirm();
    void BeginMove(const Move& move);
    /// Ask the computer for black's move and play it.
    void PlayComputer();
    void Finish();

    /// Where a square's piece stands: on the board, facing the opponent.
    engine::Vec3 CentreOf(Square square) const;
    engine::Mat4 Placement(const Piece& piece) const;
    engine::Mat4 MoverTransform(const Piece& piece) const;
    engine::Mat4 VictimTransform(const Piece& piece) const;
    engine::Mat4 Parked() const;

    void Write(engine::Scene& scene);

    Board board_;
    Assets assets_;
    std::uint32_t board_slab_ = 0;
    std::uint32_t board_surface_ = 0;
    std::uint32_t cursor_node_ = 0;
    std::uint32_t check_node_ = 0;
    std::uint32_t trail_from_node_ = 0;
    std::uint32_t trail_to_node_ = 0;
    std::uint32_t shot_node_ = 0;
    std::vector<std::uint32_t> piece_node_;
    std::vector<std::uint32_t> marker_node_;

    Square cursor_;
    int selected_ = -1;
    std::vector<Move> selected_moves_;
    Animation animation_;
    /// Set once the game is over, so the beaten general's topple is not mistaken for the
    /// start of another one.
    bool over_ = false;
    Computer computer_;
    bool computer_black_ = true;
    bool self_play_ = false;
    std::string event_;
};

}  // namespace zlong::xiangqi
