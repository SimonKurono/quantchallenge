# Quant Challenge 2025 – Strategy README

## 🔢 How the Model Works
- The dataset provides game features (`A–N`) and outcomes (`Y1`, `Y2`).
- The model maps **game state variables** (score difference, momentum, home advantage, and time) into a **win probability estimate**.  
- This mapping uses a **logistic (sigmoid) function**, ensuring probabilities stay between 0% and 100%.  
- Scaling is applied so that early-game leads are weighted less, while late-game leads and momentum shifts matter more.  
- The predicted win probability is then expressed as a **fair market value** on a 0–100 price scale:
  - `0 = certain loss`
  - `100 = certain win`
- This fair value acts as the **anchor price** the strategy compares against the market bid/ask quotes.

---

## ⚙️ How the Algorithm Works
The trading algorithm (`Strategy` class) is a **quantitative market-making system** with built-in risk control.

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

