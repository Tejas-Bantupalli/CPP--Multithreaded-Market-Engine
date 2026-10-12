FORECAST (required, recorded, does not affect your score)

The comment block at the top of your file must contain exactly one line of this form, with
nothing else on that line:

// FORECAST pnl=<number> rank=<integer> confidence=<number between 0 and 1>

  pnl         the mean PnL in dollars you expect this strategy to score when it is run
  rank        where you expect to place among the adaptive agents in this market, 1 = best
  confidence  how sure you are of those two numbers, 0.0 = a pure guess, 1.0 = certain

Example of the form (the numbers are arbitrary and carry no suggestion):

// FORECAST pnl=-3.5 rank=3 confidence=0.2

This line is compared afterwards with what actually happened. It changes nothing about how your
strategy is compiled, run, or scored, so there is no advantage in a flattering number. A
well-judged low forecast is a better answer than an optimistic high one.
