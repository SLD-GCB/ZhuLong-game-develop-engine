// 烛龙 (ZhuLong) - Chinese chess (象棋), as rules.
//
// The game's whole logic and deliberately nothing else: a board, the seven kinds of
// piece, whose turn it is, which moves are legal, and when the game is over. It knows
// nothing about a GPU, a window or a key -- the same perspective the engine's Scene has
// one layer down. A presenter turns a Board into a picture and keys into moves.
//
// The piece list is fixed for the whole game. A captured piece is marked dead and stays
// in place in the list, which is what lets a presenter keep exactly one node per piece
// and never reshuffle them (the renderer locks its node count at Prepare).

#pragma once

#include <cstdint>
#include <vector>

namespace zlong::xiangqi {

/// Nine files (columns), ten ranks (rows). Red starts on rank 0 and moves up; black
/// starts on rank 9 and moves down. The river is between ranks 4 and 5.
inline constexpr int kFiles = 9;
inline constexpr int kRanks = 10;

enum class Side : std::uint8_t { Red, Black };

inline Side Opponent(Side side) noexcept {
    return side == Side::Red ? Side::Black : Side::Red;
}

enum class Kind : std::uint8_t {
    General,   /// 帥 / 將
    Advisor,   /// 仕 / 士
    Elephant,  /// 相 / 象
    Horse,     /// 馬
    Chariot,   /// 車
    Cannon,    /// 炮
    Soldier,   /// 兵 / 卒
};

inline constexpr int kKindCount = 7;

/// How many of each kind a side starts with, indexed by Kind.
inline constexpr int kStartCounts[kKindCount] = {1, 2, 2, 2, 2, 2, 5};

const char* Name(Kind kind) noexcept;
const char* Name(Side side) noexcept;

struct Square {
    int file = 0;
    int rank = 0;
};

inline bool operator==(Square a, Square b) noexcept {
    return a.file == b.file && a.rank == b.rank;
}
inline bool operator!=(Square a, Square b) noexcept { return !(a == b); }
inline bool OnBoard(Square square) noexcept {
    return square.file >= 0 && square.file < kFiles && square.rank >= 0 &&
           square.rank < kRanks;
}

/// One piece. `alive` is false once it has been captured; the entry stays in the list.
struct Piece {
    Kind kind = Kind::Soldier;
    Side side = Side::Red;
    Square at;
    bool alive = true;
};

struct Move {
    Square from;
    Square to;
    /// True when something is standing on `to`.
    bool capture = false;
};

/// How a game stands. Chinese chess has three ways to lose and three to draw, so "over" is not
/// always "someone was mated".
enum class Outcome : std::uint8_t { Playing, RedWins, BlackWins, Draw };

/// Why the game ended.
enum class Ending : std::uint8_t {
    None,
    Checkmate,        /// 将死
    Stalemate,        /// 困毙: no legal move at all, which here is a loss and not a draw
    PerpetualCheck,   /// 长将: the loser gave check on every one of its moves in the cycle
    PerpetualAttack,  /// 长捉: the loser checked or chased on every one of its moves
    Repetition,       /// 循环, neither side at fault
    NaturalLimit,     /// 自然限着: sixty moves each without a capture
};

struct Result {
    Outcome outcome = Outcome::Playing;
    Ending why = Ending::None;
};

const char* Name(Ending ending) noexcept;

/// What a kind is worth, in the hundredths a soldier is a hundred. The rules need it to say
/// whether a threat is a chase worth ruling on; the computer needs it to decide what to take.
int PieceValue(Kind kind) noexcept;

/// Plies without a capture before the game is drawn: sixty moves by each side, which is the
/// natural limit in the Chinese rules.
inline constexpr int kNaturalLimitPlies = 120;

/// One position, and everything that follows from it.
class Board {
public:
    /// The starting position: thirty-two pieces, red to move.
    static Board Start();

    /// Build a position from an explicit piece list, with `side` to play. The list is
    /// taken as given -- the caller may hand over a position that could not be reached
    /// (a puzzle, a test), and the rules will simply report what is true of it.
    /// `since_capture` is how many plies it has been since the last capture, which a position
    /// set up mid-game has to say and a fresh one does not.
    static Board FromPieces(std::vector<Piece> pieces, Side side, int since_capture = 0);

    const std::vector<Piece>& pieces() const noexcept { return pieces_; }
    /// The living piece on a square, or nullptr.
    const Piece* At(Square square) const noexcept;
    /// Where a square's living piece sits in pieces(), or -1.
    int IndexAt(Square square) const noexcept;
    /// Where a side's general is, or a square off the board once it is gone.
    Square GeneralOf(Side side) const noexcept;

    Side to_move() const noexcept { return to_move_; }
    int ply() const noexcept { return ply_; }

    /// What the last applied move did. A presenter animates from this rather than
    /// diffing the position.
    struct LastMove {
        bool valid = false;
        Square from;
        Square to;
        Kind kind = Kind::Soldier;
        Side side = Side::Red;
        /// Index in pieces() of the piece this move took, or -1.
        int captured_index = -1;
        Square captured_at;
        Kind captured_kind = Kind::Soldier;
    };
    const LastMove& last_move() const noexcept { return last_; }

    /// Every legal move for the side to move. A side with none has lost.
    std::vector<Move> Moves() const;
    /// The legal moves of that square's piece, empty when the square holds nothing or
    /// holds the other side's piece.
    std::vector<Move> MovesFrom(Square from) const;
    /// True when `from` -> `to` is in Moves().
    bool IsLegal(Square from, Square to) const;

    /// Apply a legal move. False, and nothing changed, when it is not legal.
    bool Apply(Square from, Square to);

    /// Whether that side's general is under attack -- which includes the flying-general
    /// rule, because an opposing general sweeping an open file is exactly that.
    bool InCheck(Side side) const;

    /// How many times this exact position has now occurred, this one included. Three is a
    /// repetition.
    int repetition_count() const noexcept;

    /// Where the game stands: still playing, won, or drawn, and why. This is where every rule
    /// that is not about a single move lives -- mate, 困毙, 长将, 长捉, the repetition and the
    /// natural limit.
    Result result() const;
    /// The same, for a caller that has already generated the legal moves.
    Result result(const std::vector<Move>& legal) const;
    /// Only the endings that can be read off the position and its history -- the repetition,
    /// the 长将/长捉 judgement and the natural limit. Mate is not among them, because that takes
    /// the legal moves to see, and this is the version a search can afford to ask at every leaf.
    Result repetition_result() const;

    /// Whether the game is over, whatever the reason.
    bool Finished() const { return result().outcome != Outcome::Playing; }

private:
    /// What one ply left behind, which is what a repetition is judged from: the position, who
    /// moved into it, and whether that move gave check or chased.
    struct HistoryEntry {
        std::uint64_t key = 0;
        Side moved = Side::Red;
        bool check = false;
        bool chase = false;
    };

    /// A hash of the living pieces, their squares and whose turn it is. Two positions with the
    /// same key are the same position for the repetition rule, whatever route each took.
    std::uint64_t Key() const noexcept;
    /// Whether the piece now standing on `at` threatens to win an enemy piece -- which is what
    /// 捉 means, and what a chase that repeats loses by. A soldier or a general is never what a
    /// chase is about, and a piece is only chased if it is undefended or worth more than its
    /// attacker.
    bool Chases(Square at) const;
    /// Whether `side` has a piece defending `square`.
    bool Defended(Square square, Side side) const;
    /// Append the position just reached to the record.
    void Record(Side moved, Square at);

    /// Whether the piece on `from` could capture on `to`, ignoring whose turn it is.
    /// This is what check detection is built from.
    bool Attacks(Square from, Square to) const;

    std::vector<Move> PseudoMovesFrom(Square from) const;
    /// Whether moving `from` -> `to` leaves the mover's own general attacked.
    bool LeavesOwnGeneralSafe(Square from, Square to) const;
    /// How many living pieces stand strictly between two squares on the same file or
    /// rank. Meaningless when they are not.
    int CountBetween(Square a, Square b) const;

    /// Mutable on purpose. `LeavesOwnGeneralSafe` makes a move and unmakes it rather than
    /// copying the position, because the move generator asks that question once per candidate
    /// move and once the repetition rule put a move history in here, a copy per candidate meant
    /// copying the history too -- which took the computer from eighty milliseconds a move to
    /// eleven hundred. The mutation is always undone before the method returns, so a Board's
    /// value never actually changes.
    mutable std::vector<Piece> pieces_;
    Side to_move_ = Side::Red;
    int ply_ = 0;
    LastMove last_;
    /// One entry per position reached, the starting one included.
    std::vector<HistoryEntry> history_;
    int since_capture_ = 0;
};

}  // namespace zlong::xiangqi
