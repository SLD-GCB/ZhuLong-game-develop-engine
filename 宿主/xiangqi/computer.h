// 烛龙 (ZhuLong) - the opponent.
//
// A negamax search with alpha-beta pruning and a material evaluation, plus one positional
// term (a soldier that has crossed the river). That is deliberately a small amount of
// chess: it takes what is left hanging, it does not hand pieces over, and it sees a mate
// within its horizon. Enough for the other side of a board.
//
// It cannot cheat, and that is a property worth stating: every move it plays comes out of
// `Board::Moves()`, so it is exactly as bound by the rules as a person is.

#pragma once

#include <optional>

#include "rules.h"

namespace zlong::xiangqi {

class Computer {
public:
    /// Plies to look ahead, counting the move it plays as one. Three sees a threat and the
    /// answer to it, which is where the blunders stop, and costs 6-80 ms a move measured
    /// over a self-played game -- short enough that nobody notices it thinking.
    ///
    /// Four was measured at 3.3-4.2 seconds a move, which is a freeze in the middle of a
    /// game. Raising it means making the move generator cheaper first: `Board::Moves` copies
    /// the whole position once per candidate move.
    static constexpr int kDepth = 3;

    /// The move to play for whoever is to move, or nothing when that side has none.
    std::optional<Move> Choose(const Board& board);

    /// How long the last Choose took, in milliseconds. Printed rather than guessed at:
    /// "the computer takes a moment" is a claim, and this is the measurement.
    double last_think_ms() const noexcept { return think_ms_; }

private:
    double think_ms_ = 0.0;
};

}  // namespace zlong::xiangqi
