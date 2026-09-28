#include "rules.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace zlong::xiangqi {

namespace {

/// A side's palace: files 3..5 and the three ranks at that side's end.
bool InPalace(Side side, Square square) {
    if (square.file < 3 || square.file > 5) {
        return false;
    }
    return side == Side::Red ? square.rank <= 2 : square.rank >= 7;
}

/// A side's own half, which is as far as an elephant may go.
bool OnOwnHalf(Side side, Square square) {
    return side == Side::Red ? square.rank <= 4 : square.rank >= 5;
}

/// Whether a soldier of that side has crossed the river, and may therefore step
/// sideways.
bool CrossedRiver(Side side, Square square) {
    return side == Side::Red ? square.rank >= 5 : square.rank <= 4;
}

int Sign(int value) { return value > 0 ? 1 : (value < 0 ? -1 : 0); }

}  // namespace

const char* Name(Kind kind) noexcept {
    switch (kind) {
        case Kind::General: return "general";
        case Kind::Advisor: return "advisor";
        case Kind::Elephant: return "elephant";
        case Kind::Horse: return "horse";
        case Kind::Chariot: return "chariot";
        case Kind::Cannon: return "cannon";
        case Kind::Soldier: return "soldier";
    }
    return "?";
}

const char* Name(Side side) noexcept { return side == Side::Red ? "red" : "black"; }

const char* Name(Ending ending) noexcept {
    switch (ending) {
        case Ending::None: return "playing";
        case Ending::Checkmate: return "checkmate";
        case Ending::Stalemate: return "no legal move";
        case Ending::PerpetualCheck: return "perpetual check";
        case Ending::PerpetualAttack: return "perpetual chase";
        case Ending::Repetition: return "repetition";
        case Ending::NaturalLimit: return "sixty moves without a capture";
    }
    return "?";
}

int PieceValue(Kind kind) noexcept {
    switch (kind) {
        // Never taken: leaving your own general attacked is not a legal move, so a position
        // with one missing cannot arise.
        case Kind::General: return 0;
        case Kind::Chariot: return 900;
        case Kind::Cannon: return 450;
        case Kind::Horse: return 400;
        case Kind::Elephant: return 200;
        case Kind::Advisor: return 200;
        case Kind::Soldier: return 100;
    }
    return 0;
}

Board Board::Start() {
    Board board;
    const Kind back[kFiles] = {Kind::Chariot, Kind::Horse, Kind::Elephant, Kind::Advisor,
                               Kind::General, Kind::Advisor, Kind::Elephant, Kind::Horse,
                               Kind::Chariot};
    for (int file = 0; file < kFiles; ++file) {
        board.pieces_.push_back(Piece{back[file], Side::Red, Square{file, 0}, true});
        board.pieces_.push_back(Piece{back[file], Side::Black, Square{file, 9}, true});
    }
    for (const int file : {1, 7}) {
        board.pieces_.push_back(Piece{Kind::Cannon, Side::Red, Square{file, 2}, true});
        board.pieces_.push_back(Piece{Kind::Cannon, Side::Black, Square{file, 7}, true});
    }
    for (const int file : {0, 2, 4, 6, 8}) {
        board.pieces_.push_back(Piece{Kind::Soldier, Side::Red, Square{file, 3}, true});
        board.pieces_.push_back(Piece{Kind::Soldier, Side::Black, Square{file, 6}, true});
    }
    board.Record(Side::Black, Square{-1, -1});  // the position before anyone has moved
    return board;
}

Board Board::FromPieces(std::vector<Piece> pieces, Side side, int since_capture) {
    Board board;
    board.pieces_ = std::move(pieces);
    board.to_move_ = side;
    board.since_capture_ = since_capture;
    board.Record(Side::Black, Square{-1, -1});  // the position before anyone has moved
    return board;
}

std::uint64_t Board::Key() const noexcept {
    // FNV-1a over the living pieces in list order, which is stable, and then whose turn it is.
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](std::uint64_t value) {
        h ^= value;
        h *= 1099511628211ull;
    };
    for (const Piece& piece : pieces_) {
        if (!piece.alive) {
            continue;
        }
        mix(static_cast<std::uint64_t>(piece.kind));
        mix(static_cast<std::uint64_t>(piece.side));
        mix(static_cast<std::uint64_t>(piece.at.file));
        mix(static_cast<std::uint64_t>(piece.at.rank));
    }
    mix(static_cast<std::uint64_t>(to_move_));
    return h;
}

int Board::repetition_count() const noexcept {
    const std::uint64_t key = Key();
    int count = 0;
    for (const HistoryEntry& entry : history_) {
        if (entry.key == key) {
            ++count;
        }
    }
    return count;
}

bool Board::Defended(Square square, Side side) const {
    for (const Piece& piece : pieces_) {
        if (!piece.alive || piece.side != side || piece.at == square) {
            continue;
        }
        if (Attacks(piece.at, square)) {
            return true;
        }
    }
    return false;
}

bool Board::Chases(Square at) const {
    const int index = IndexAt(at);
    if (index < 0) {
        return false;
    }
    const Piece& attacker = pieces_[static_cast<std::size_t>(index)];
    for (const Piece& piece : pieces_) {
        if (!piece.alive || piece.side == attacker.side) {
            continue;
        }
        // A soldier or a general is never what a chase is about -- you do not chase a pawn --
        // and this is the filter that keeps the test cheap as well as correct.
        if (piece.kind == Kind::Soldier || piece.kind == Kind::General) {
            continue;
        }
        if (!Attacks(at, piece.at)) {
            continue;
        }
        // A threat to win material: the piece is undefended, or worth more than its attacker.
        if (!Defended(piece.at, piece.side) ||
            PieceValue(piece.kind) > PieceValue(attacker.kind)) {
            return true;
        }
    }
    return false;
}

void Board::Record(Side moved, Square at) {
    HistoryEntry entry;
    entry.key = Key();
    entry.moved = moved;
    // After the move it is the opponent who is to move, so this asks the question that matters:
    // did the move just played give check?
    entry.check = InCheck(to_move_);
    entry.chase = OnBoard(at) ? Chases(at) : false;
    history_.push_back(entry);
}

Result Board::repetition_result() const {
    if (since_capture_ >= kNaturalLimitPlies) {
        return Result{Outcome::Draw, Ending::NaturalLimit};
    }
    if (repetition_count() < 3) {
        return Result{};
    }

    // The position has come round three times. Who is at fault is decided by the moves *inside*
    // the cycle: a side that gave check every time loses (长将), and so does one that checked or
    // chased every time (长捉), while a cycle neither side drove is a draw.
    const std::uint64_t key = Key();
    std::size_t first = history_.size();
    for (std::size_t index = 0; index < history_.size(); ++index) {
        if (history_[index].key == key) {
            first = index;
            break;
        }
    }
    if (first >= history_.size()) {
        return Result{Outcome::Draw, Ending::Repetition};
    }

    bool moved[2] = {false, false};
    bool checked_every[2] = {true, true};
    bool attacked_every[2] = {true, true};
    for (std::size_t index = first + 1; index < history_.size(); ++index) {
        const HistoryEntry& entry = history_[index];
        const auto side = static_cast<std::size_t>(entry.moved);
        moved[side] = true;
        checked_every[side] = checked_every[side] && entry.check;
        attacked_every[side] = attacked_every[side] && (entry.check || entry.chase);
    }
    const auto red = static_cast<std::size_t>(Side::Red);
    const auto black = static_cast<std::size_t>(Side::Black);
    const bool red_check = moved[red] && checked_every[red];
    const bool black_check = moved[black] && checked_every[black];
    const bool red_attack = moved[red] && attacked_every[red];
    const bool black_attack = moved[black] && attacked_every[black];

    if (red_check != black_check) {
        return Result{red_check ? Outcome::BlackWins : Outcome::RedWins, Ending::PerpetualCheck};
    }
    if (!red_check && !black_check && red_attack != black_attack) {
        return Result{red_attack ? Outcome::BlackWins : Outcome::RedWins, Ending::PerpetualAttack};
    }
    return Result{Outcome::Draw, Ending::Repetition};
}

Result Board::result(const std::vector<Move>& legal) const {
    // A side with nowhere to go has lost, in check or not: 困毙 is a loss in this game and not
    // a draw.
    if (legal.empty()) {
        return Result{to_move_ == Side::Red ? Outcome::BlackWins : Outcome::RedWins,
                      InCheck(to_move_) ? Ending::Checkmate : Ending::Stalemate};
    }
    return repetition_result();
}

Result Board::result() const { return result(Moves()); }

int Board::IndexAt(Square square) const noexcept {
    for (std::size_t index = 0; index < pieces_.size(); ++index) {
        if (pieces_[index].alive && pieces_[index].at == square) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

const Piece* Board::At(Square square) const noexcept {
    const int index = IndexAt(square);
    return index < 0 ? nullptr : &pieces_[static_cast<std::size_t>(index)];
}

Square Board::GeneralOf(Side side) const noexcept {
    for (const Piece& piece : pieces_) {
        if (piece.alive && piece.side == side && piece.kind == Kind::General) {
            return piece.at;
        }
    }
    return Square{-1, -1};
}

int Board::CountBetween(Square a, Square b) const {
    int count = 0;
    if (a.file == b.file) {
        const int lo = std::min(a.rank, b.rank);
        const int hi = std::max(a.rank, b.rank);
        for (int rank = lo + 1; rank < hi; ++rank) {
            if (IndexAt(Square{a.file, rank}) >= 0) {
                ++count;
            }
        }
    } else if (a.rank == b.rank) {
        const int lo = std::min(a.file, b.file);
        const int hi = std::max(a.file, b.file);
        for (int file = lo + 1; file < hi; ++file) {
            if (IndexAt(Square{file, a.rank}) >= 0) {
                ++count;
            }
        }
    }
    return count;
}

bool Board::Attacks(Square from, Square to) const {
    if (from == to) {
        return false;
    }
    const int index = IndexAt(from);
    if (index < 0) {
        return false;
    }
    const Piece& piece = pieces_[static_cast<std::size_t>(index)];
    const int df = to.file - from.file;
    const int dr = to.rank - from.rank;

    switch (piece.kind) {
        case Kind::Chariot:
            return (df == 0 || dr == 0) && CountBetween(from, to) == 0;
        case Kind::Cannon:
            return (df == 0 || dr == 0) && CountBetween(from, to) == 1;
        case Kind::Horse: {
            const int adf = std::abs(df);
            const int adr = std::abs(dr);
            if (!((adf == 1 && adr == 2) || (adf == 2 && adr == 1))) {
                return false;
            }
            const Square leg{from.file + (adf == 2 ? Sign(df) : 0),
                             from.rank + (adr == 2 ? Sign(dr) : 0)};
            return IndexAt(leg) < 0;
        }
        case Kind::Elephant: {
            if (std::abs(df) != 2 || std::abs(dr) != 2) {
                return false;
            }
            if (!OnOwnHalf(piece.side, to)) {
                return false;
            }
            const Square eye{from.file + Sign(df), from.rank + Sign(dr)};
            return IndexAt(eye) < 0;
        }
        case Kind::Advisor:
            return std::abs(df) == 1 && std::abs(dr) == 1 && InPalace(piece.side, to);
        case Kind::General:
            // One step inside the palace, or the flying-general sweep down an open
            // file -- the sweep is the only way a general reaches the other one, and
            // modelling it as an attack is what makes facing generals illegal.
            if (std::abs(df) + std::abs(dr) == 1) {
                return InPalace(piece.side, to);
            }
            return df == 0 && CountBetween(from, to) == 0;
        case Kind::Soldier: {
            const int forward = piece.side == Side::Red ? 1 : -1;
            if (df == 0 && dr == forward) {
                return true;
            }
            return CrossedRiver(piece.side, from) && dr == 0 && std::abs(df) == 1;
        }
    }
    return false;
}

bool Board::InCheck(Side side) const {
    const Square general = GeneralOf(side);
    if (!OnBoard(general)) {
        return true;
    }
    for (const Piece& piece : pieces_) {
        if (!piece.alive || piece.side == side) {
            continue;
        }
        if (Attacks(piece.at, general)) {
            return true;
        }
    }
    return false;
}

bool Board::LeavesOwnGeneralSafe(Square from, Square to) const {
    // Make the move, ask about the general, unmake it. Only the two pieces it touches are
    // disturbed, and the alternatives -- a copy of the position, or of its piece list -- cost an
    // allocation per candidate move.
    const int mover = IndexAt(from);
    if (mover < 0) {
        return false;
    }
    const Side side = pieces_[static_cast<std::size_t>(mover)].side;
    const int taken = IndexAt(to);
    const Square was = pieces_[static_cast<std::size_t>(mover)].at;
    if (taken >= 0) {
        pieces_[static_cast<std::size_t>(taken)].alive = false;
    }
    pieces_[static_cast<std::size_t>(mover)].at = to;
    const bool safe = !InCheck(side);
    pieces_[static_cast<std::size_t>(mover)].at = was;
    if (taken >= 0) {
        pieces_[static_cast<std::size_t>(taken)].alive = true;
    }
    return safe;
}

std::vector<Move> Board::PseudoMovesFrom(Square from) const {
    std::vector<Move> moves;
    const int index = IndexAt(from);
    if (index < 0) {
        return moves;
    }
    const Piece& piece = pieces_[static_cast<std::size_t>(index)];
    const Side side = piece.side;

    const auto push = [&](Square to) {
        if (!OnBoard(to)) {
            return;
        }
        const int at = IndexAt(to);
        if (at >= 0 && pieces_[static_cast<std::size_t>(at)].side == side) {
            return;
        }
        moves.push_back(Move{from, to, at >= 0});
    };

    const int orthogonal[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    switch (piece.kind) {
        case Kind::Chariot:
            for (const auto& step : orthogonal) {
                Square cursor = from;
                for (;;) {
                    cursor.file += step[0];
                    cursor.rank += step[1];
                    if (!OnBoard(cursor)) {
                        break;
                    }
                    const int at = IndexAt(cursor);
                    if (at < 0) {
                        moves.push_back(Move{from, cursor, false});
                        continue;
                    }
                    if (pieces_[static_cast<std::size_t>(at)].side != side) {
                        moves.push_back(Move{from, cursor, true});
                    }
                    break;
                }
            }
            break;
        case Kind::Cannon:
            for (const auto& step : orthogonal) {
                Square cursor = from;
                bool screened = false;
                for (;;) {
                    cursor.file += step[0];
                    cursor.rank += step[1];
                    if (!OnBoard(cursor)) {
                        break;
                    }
                    const int at = IndexAt(cursor);
                    if (!screened) {
                        // Before the screen a cannon moves like a chariot but cannot take.
                        if (at < 0) {
                            moves.push_back(Move{from, cursor, false});
                        } else {
                            screened = true;
                        }
                        continue;
                    }
                    if (at >= 0) {
                        // Past the screen, the first piece met is the only capture.
                        if (pieces_[static_cast<std::size_t>(at)].side != side) {
                            moves.push_back(Move{from, cursor, true});
                        }
                        break;
                    }
                }
            }
            break;
        case Kind::Horse: {
            const int offsets[8][2] = {{1, 2},  {2, 1},   {2, -1}, {1, -2},
                                       {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};
            for (const auto& offset : offsets) {
                const Square to{from.file + offset[0], from.rank + offset[1]};
                if (!OnBoard(to)) {
                    continue;
                }
                const Square leg{from.file + (std::abs(offset[0]) == 2 ? Sign(offset[0]) : 0),
                                 from.rank + (std::abs(offset[1]) == 2 ? Sign(offset[1]) : 0)};
                if (IndexAt(leg) >= 0) {
                    continue;
                }
                push(to);
            }
            break;
        }
        case Kind::Elephant: {
            const int offsets[4][2] = {{2, 2}, {2, -2}, {-2, 2}, {-2, -2}};
            for (const auto& offset : offsets) {
                const Square to{from.file + offset[0], from.rank + offset[1]};
                if (!OnBoard(to) || !OnOwnHalf(side, to)) {
                    continue;
                }
                const Square eye{from.file + Sign(offset[0]), from.rank + Sign(offset[1])};
                if (IndexAt(eye) >= 0) {
                    continue;
                }
                push(to);
            }
            break;
        }
        case Kind::Advisor: {
            const int offsets[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
            for (const auto& offset : offsets) {
                const Square to{from.file + offset[0], from.rank + offset[1]};
                if (!OnBoard(to) || !InPalace(side, to)) {
                    continue;
                }
                push(to);
            }
            break;
        }
        case Kind::General:
            for (const auto& step : orthogonal) {
                const Square to{from.file + step[0], from.rank + step[1]};
                if (!OnBoard(to) || !InPalace(side, to)) {
                    continue;
                }
                push(to);
            }
            break;
        case Kind::Soldier: {
            const int forward = side == Side::Red ? 1 : -1;
            push(Square{from.file, from.rank + forward});
            if (CrossedRiver(side, from)) {
                push(Square{from.file + 1, from.rank});
                push(Square{from.file - 1, from.rank});
            }
            break;
        }
    }
    return moves;
}

std::vector<Move> Board::MovesFrom(Square from) const {
    std::vector<Move> legal;
    const int index = IndexAt(from);
    if (index < 0 || pieces_[static_cast<std::size_t>(index)].side != to_move_) {
        return legal;
    }
    for (const Move& move : PseudoMovesFrom(from)) {
        if (LeavesOwnGeneralSafe(move.from, move.to)) {
            legal.push_back(move);
        }
    }
    return legal;
}

std::vector<Move> Board::Moves() const {
    std::vector<Move> all;
    for (const Piece& piece : pieces_) {
        if (!piece.alive || piece.side != to_move_) {
            continue;
        }
        for (const Move& move : MovesFrom(piece.at)) {
            all.push_back(move);
        }
    }
    return all;
}

bool Board::IsLegal(Square from, Square to) const {
    for (const Move& move : MovesFrom(from)) {
        if (move.to == to) {
            return true;
        }
    }
    return false;
}

bool Board::Apply(Square from, Square to) {
    if (!IsLegal(from, to)) {
        return false;
    }
    const int mover = IndexAt(from);
    last_ = LastMove{};
    last_.valid = true;
    last_.from = from;
    last_.to = to;
    last_.kind = pieces_[static_cast<std::size_t>(mover)].kind;
    last_.side = pieces_[static_cast<std::size_t>(mover)].side;

    const int taken = IndexAt(to);
    if (taken >= 0) {
        last_.captured_index = taken;
        last_.captured_at = to;
        last_.captured_kind = pieces_[static_cast<std::size_t>(taken)].kind;
        pieces_[static_cast<std::size_t>(taken)].alive = false;
    }
    pieces_[static_cast<std::size_t>(mover)].at = to;
    to_move_ = Opponent(to_move_);
    ++ply_;
    since_capture_ = taken >= 0 ? 0 : since_capture_ + 1;
    Record(last_.side, to);
    return true;
}

}  // namespace zlong::xiangqi
