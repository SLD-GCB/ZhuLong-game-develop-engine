#include "computer.h"

#include <algorithm>
#include <chrono>

namespace zlong::xiangqi {

namespace {

/// Big enough to be past any material score, so a mate is never traded for a piece.
constexpr int kMate = 1000000;
constexpr int kInfinity = 2000000;

/// The score for `side`, in hundredths of a soldier. Material, and the one positional fact
/// that matters at every stage of this game: a soldier that has crossed the river is worth
/// more, and more again the further up it has come.
int Evaluate(const Board& board, Side side) {
    int score = 0;
    for (const Piece& piece : board.pieces()) {
        if (!piece.alive) {
            continue;
        }
        int value = PieceValue(piece.kind);
        if (piece.kind == Kind::Soldier) {
            const bool crossed =
                piece.side == Side::Red ? piece.at.rank >= 5 : piece.at.rank <= 4;
            const int advance = piece.side == Side::Red ? piece.at.rank : (kRanks - 1 - piece.at.rank);
            if (crossed) {
                value += 100;
            }
            value += advance * 6;
        }
        score += piece.side == side ? value : -value;
    }
    return score;
}

/// Negamax: the value of a position is the value of the best move from it, refused in sign
/// for whoever is to move. The score returned is always from the point of view of the side
/// to move at `board`.
int Search(const Board& board, int depth, int alpha, int beta) {
    if (depth <= 0) {
        // At the horizon only the endings that need no move list are worth looking for -- a
        // repetition, which is what a lost-by-长将 game looks like from here. Asking the full
        // question would mean generating every legal move at every leaf, and that alone took
        // the search from eighty milliseconds to eleven hundred.
        const Result ruled = board.repetition_result();
        if (ruled.outcome == Outcome::Draw) {
            return 0;
        }
        if (ruled.outcome != Outcome::Playing) {
            const bool mover_wins =
                (board.to_move() == Side::Red) == (ruled.outcome == Outcome::RedWins);
            return mover_wins ? kMate : -kMate;
        }
        return Evaluate(board, board.to_move());
    }

    const auto moves = board.Moves();
    // A position that is already over is scored as over however deep the search has come: a
    // loss is a loss even if it is by 长将 rather than by mate, and a draw is nothing.
    const Result result = board.result(moves);
    if (result.outcome != Outcome::Playing) {
        if (result.outcome == Outcome::Draw) {
            return 0;
        }
        const bool mover_wins =
            (board.to_move() == Side::Red) == (result.outcome == Outcome::RedWins);
        return mover_wins ? kMate : -kMate;
    }
    int best = -kInfinity;
    for (const Move& move : moves) {
        Board next = board;
        next.Apply(move.from, move.to);
        const int score = -Search(next, depth - 1, -beta, -alpha);
        best = std::max(best, score);
        alpha = std::max(alpha, score);
        if (alpha >= beta) {
            break;
        }
    }
    return best;
}

}  // namespace

std::optional<Move> Computer::Choose(const Board& board) {
    const auto started = std::chrono::steady_clock::now();
    const auto moves = board.Moves();
    if (moves.empty() || board.result(moves).outcome != Outcome::Playing) {
        // Nothing to play, whether because there is no legal move or because the game is
        // already over for one of the other reasons.
        think_ms_ = 0.0;
        return std::nullopt;
    }

    // The root is done by hand rather than through Search, because what is wanted is the
    // *move*, not the score.
    Move best = moves.front();
    int best_score = -kInfinity;
    int alpha = -kInfinity;
    for (const Move& move : moves) {
        Board next = board;
        next.Apply(move.from, move.to);
        const int score = -Search(next, kDepth - 1, -kInfinity, -alpha);
        if (score > best_score) {
            best_score = score;
            best = move;
        }
        alpha = std::max(alpha, best_score);
    }
    think_ms_ =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    return best;
}

}  // namespace zlong::xiangqi
