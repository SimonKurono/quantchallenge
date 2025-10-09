# Quant Challenge 2025 – Strategy README
## Part I — Research: Predictive Modeling
The research phase focused on building a **supervised learning model** to estimate in-game outcome probabilities.

- **Dataset:** Features `A–N` and target variables `Y1`, `Y2`.  
- **Goal:** Predict continuous win-probability estimates for downstream trading.  
- **Model:** `XGBoostRegressor` tuned via `RandomizedSearchCV` over parameters such as `learning_rate`, `max_depth`, `subsample`, and `colsample_bytree`.  
- **Feature Engineering:**  
  - Rolling **means** and **standard deviations** to capture momentum and volatility.  
  - **Interaction terms** and **temporal flags** (early/mid/late phase) for contextual sensitivity.  
  - Standardization (z-score) to stabilize across varying scales.  
- **Validation:** Time-series cross-validation with early stopping; residual autocorrelation checks and feature-importance tracking for interpretability.  

---

## Part II — Algorithmic Trading Strategy
The trading algorithm (`kwokker-algo.py`) file is a **quantitative market-making system** with built-in risk control.

1. **Order Book Tracking**  
   - Keeps a minimal local view of the **best bid and best ask** with available liquidity.  
   - Continuously updates mid-price, spread, and market depth.

2. **Edge Calculation**  
   - Computes `edge = fair_value - market_price`.  
   - If the edge is larger than a threshold, a trade opportunity exists.  
   - The threshold **tightens late-game**, requiring smaller edges to justify trades.

3. **Execution Logic**  
   - **Aggressive mode**: Crosses the spread (IOC limit orders) when the spread is small and the edge is strong.  
   - **Passive mode**: Posts a single resting order just inside the spread if the edge is moderate.  
   - Always cancels stale working orders to avoid overexposure.

4. **Risk Management**  
   - Position is capped at a maximum size (`MAX_POS`).  
   - Trade sizing scales with both **edge size** and **time urgency** (larger sizes in late game).  
   - Automatically **flattens inventory** shortly before game end or on `END_GAME`.

5. **Event Handling**  
   - Adjusts momentum and lead difference when scoring or turnovers occur.  
   - Treats **3-point shots, turnovers, and fouls in late-game** as **high-impact events**, increasing trading aggressiveness.  
   - If the game ends, all positions are closed and state is reset.

