"""
Quant Challenge 2025

Algorithmic strategy template
"""

from enum import Enum
from typing import Optional, List, Tuple
import math
import time

class Side(Enum):
    BUY = 0
    SELL = 1

class Ticker(Enum):
    # TEAM_A (home team)
    TEAM_A = 0

# The platform provides these at runtime; stubs here for type hints.
def place_market_order(side: Side, ticker: Ticker, quantity: float) -> None: ...
def place_limit_order(side: Side, ticker: Ticker, quantity: float, price: float, ioc: bool = False) -> int: ...
def cancel_order(ticker: Ticker, order_id: int) -> bool: ...

class Strategy:
    """Simple-but-clever baseline:
    - Maintains a tiny local order book (best bid/ask).
    - Maps score/time/momentum -> win prob -> fair value in [0,100].
    - Trades on clear edges (threshold tightens late or on high-impact events).
    - Crosses only when spread is sane; otherwise rests a single passive.
    - Position caps + late-game auto-flatten + END_GAME reset.
    """

    # ---- Tunables ----
    MAX_POS = 800.0              # absolute contracts
    RISK_PCT = 0.007             # per-entry capital fraction
    MAX_SPREAD_TO_CROSS = 2.0    # price points
    PASSIVE_IMPROVE = 0.1        # one tick
    MIN_QTY_LVL = 1.0            # ignore dust levels
    HOME_ADV = 1.0               # points of home edge
    BASE_EDGE = 0.9              # base mispricing threshold (tightens late)
    INIT_COOLDOWN = 3.0          # seconds after reset before trading
    CLOSE_OUT_BUFFER = 2.0       # seconds before END_GAME: flatten
    GAME_LEN1 = 2400.0
    GAME_LEN2 = 2880.0

    def reset_state(self) -> None:
        # Order book (only track visible bests with qty filter)
        self._bids = {}  # price -> qty
        self._asks = {}  # price -> qty

        # Account
        self.capital = 100_000.0
        self.pos = 0.0

        # Game state
        self.t = self.GAME_LEN2
        self.home = 0
        self.away = 0
        self.lead = 0.0
        self.momentum = 0.0   # EMA of lead deltas
        self._seen_event = False

        # Admin
        self._start = time.monotonic()
        self._work_bid: Optional[int] = None
        self._work_ask: Optional[int] = None

    def __init__(self) -> None:
        self.reset_state()

    # ---------- Helpers (inside class) ----------
    @staticmethod
    def _clamp_price(p: float) -> float:
        return min(100.0, max(0.0, p))

    @staticmethod
    def _sigmoid(x: float) -> float:
        return 1.0 / (1.0 + math.exp(-x))

    def _best_bid(self) -> Optional[float]:
        if not self._bids: return None
        for p in sorted(self._bids.keys(), reverse=True):
            if self._bids[p] >= self.MIN_QTY_LVL:
                return p
        return None

    def _best_ask(self) -> Optional[float]:
        if not self._asks: return None
        for p in sorted(self._asks.keys()):
            if self._asks[p] >= self.MIN_QTY_LVL:
                return p
        return None

    def _mid(self) -> Optional[float]:
        bb, aa = self._best_bid(), self._best_ask()
        if bb is None or aa is None:
            return None
        return 0.5 * (bb + aa)

    def _edge_threshold(self) -> float:
        # Tighten late using a smooth tanh schedule
        late_fac = 1.0 - 0.55 * (1.0 - math.tanh(max(self.t, 0.0) / 600.0))
        return max(0.2, self.BASE_EDGE * late_fac)

    def _win_prob(self) -> float:
        t = max(self.t, 0.0)
        scale = 1.0 / math.sqrt((t / 60.0) + 1.0)         # minutes-ish
        xlead = self.lead * scale
        xhome = self.HOME_ADV * scale
        xmom  = self.momentum * (1.0 + (1.0 - math.tanh(t / 600.0)))  # a bit more late
        logit = 0.18 * xlead + 0.20 * xhome + 0.10 * xmom
        return max(0.01, min(0.99, self._sigmoid(logit)))

    def _fair(self) -> float:
        return 100.0 * self._win_prob()

    def _size_for_edge(self, edge: float, ref_price: float) -> float:
        if ref_price <= 0.0: return 0.0
        budget = self.capital * self.RISK_PCT
        base = max(1.0, budget / max(1.0, ref_price))
        # modestly scale with |edge|
        sz = base * (0.5 + min(1.5, abs(edge) / 2.0))
        # soft time urgency (1→~2 as t→0)
        urg = 1.0 + (1.0 - math.tanh(max(self.t, 0.0) / 800.0))
        sz *= urg
        # cap
        sz = min(sz, self.MAX_POS * 0.25)
        return max(0.0, math.floor(sz))

    def _cancel_working(self) -> None:
        if self._work_bid is not None:
            cancel_order(Ticker.TEAM_A, self._work_bid)
            self._work_bid = None
        if self._work_ask is not None:
            cancel_order(Ticker.TEAM_A, self._work_ask)
            self._work_ask = None

    def _flatten_all(self) -> None:
        self._cancel_working()
        if abs(self.pos) >= 1.0:
            if self.pos > 0:
                place_market_order(Side.SELL, Ticker.TEAM_A, math.floor(self.pos))
            else:
                place_market_order(Side.BUY,  Ticker.TEAM_A, math.floor(-self.pos))

    def _try_trade(self, high_impact: bool = False) -> None:
        # init cooldown
        if time.monotonic() - self._start < self.INIT_COOLDOWN:
            return
        bb, aa, md = self._best_bid(), self._best_ask(), self._mid()
        if bb is None or aa is None or md is None:
            return

        spread = max(0.0, aa - bb)
        fair = self._fair()
        thr = self._edge_threshold()
        e_buy  = fair - aa          # positive → buy
        e_sell = bb - fair          # positive → sell

        # Late-game inventory safety
        if self.t < 60.0:
            if self.pos > 0.5 and fair < bb:
                q = math.floor(max(1.0, self.pos * 0.25))
                place_market_order(Side.SELL, Ticker.TEAM_A, q)
                return
            if self.pos < -0.5 and fair > aa:
                q = math.floor(max(1.0, -self.pos * 0.25))
                place_market_order(Side.BUY, Ticker.TEAM_A, q)
                return

        allow_cross = (spread <= self.MAX_SPREAD_TO_CROSS) or high_impact

        # Cross the spread (IOC) when edge is strong
        if allow_cross and e_buy > thr and self.pos < self.MAX_POS:
            q = min(self._size_for_edge(e_buy, md), self.MAX_POS - self.pos)
            if q >= 1.0:
                self._cancel_working()
                place_limit_order(Side.BUY, Ticker.TEAM_A, q, aa, True)
                return

        if allow_cross and e_sell > thr and self.pos > -self.MAX_POS:
            q = min(self._size_for_edge(e_sell, md), self.pos + self.MAX_POS)
            if q >= 1.0:
                self._cancel_working()
                place_limit_order(Side.SELL, Ticker.TEAM_A, q, bb, True)
                return

        # Otherwise: maintain a single passive on the dominant side
        if e_buy > e_sell and e_buy > thr and self.pos < self.MAX_POS:
            px = self._clamp_price(bb + self.PASSIVE_IMPROVE)
            q  = min(self._size_for_edge(e_buy, md), self.MAX_POS - self.pos)
            if q >= 1.0:
                if self._work_bid is not None:
                    cancel_order(Ticker.TEAM_A, self._work_bid)
                if self._work_ask is not None:
                    cancel_order(Ticker.TEAM_A, self._work_ask); self._work_ask = None
                oid = place_limit_order(Side.BUY, Ticker.TEAM_A, q, px, False)
                if isinstance(oid, int) and oid >= 0:
                    self._work_bid = oid
        elif e_sell > thr and self.pos > -self.MAX_POS:
            px = self._clamp_price(aa - self.PASSIVE_IMPROVE)
            q  = min(self._size_for_edge(e_sell, md), self.pos + self.MAX_POS)
            if q >= 1.0:
                if self._work_ask is not None:
                    cancel_order(Ticker.TEAM_A, self._work_ask)
                if self._work_bid is not None:
                    cancel_order(Ticker.TEAM_A, self._work_bid); self._work_bid = None
                oid = place_limit_order(Side.SELL, Ticker.TEAM_A, q, px, False)
                if isinstance(oid, int) and oid >= 0:
                    self._work_ask = oid
        else:
            self._cancel_working()

    # ---------- Engine callbacks ----------
    def on_trade_update(self, ticker: Ticker, side: Side, quantity: float, price: float) -> None:
        # Optional: print(f"PRINT {price} x {quantity}")
        pass

    def on_orderbook_update(self, ticker: Ticker, side: Side, quantity: float, price: float) -> None:
        if ticker != Ticker.TEAM_A:
            return
        price = self._clamp_price(price)
        if side == Side.BUY:
            if quantity <= 0.0:
                self._bids.pop(price, None)
            else:
                self._bids[price] = quantity
        else:
            if quantity <= 0.0:
                self._asks.pop(price, None)
            else:
                self._asks[price] = quantity
        self._try_trade(False)

    def on_account_update(
        self,
        ticker: Ticker,
        side: Side,
        price: float,
        quantity: float,
        capital_remaining: float,
    ) -> None:
        self.capital = capital_remaining
        # Convention: +qty for filled buys, -qty for sells (matches most engines)
        self.pos += quantity if side == Side.BUY else -quantity
        # If a passive filled, remove the sibling on that side
        if self.pos > 0 and self._work_bid is not None:
            cancel_order(Ticker.TEAM_A, self._work_bid); self._work_bid = None
        if self.pos < 0 and self._work_ask is not None:
            cancel_order(Ticker.TEAM_A, self._work_ask); self._work_ask = None

    def on_game_event_update(
        self,
        event_type: str,
        home_away: str,
        home_score: int,
        away_score: int,
        player_name: Optional[str],
        substituted_player_name: Optional[str],
        shot_type: Optional[str],
        assist_player: Optional[str],
        rebound_type: Optional[str],
        coordinate_x: Optional[float],
        coordinate_y: Optional[float],
        time_seconds: Optional[float]
    ) -> None:
        # Time/format inference
        if time_seconds is not None:
            t = float(time_seconds)
            if t <= self.GAME_LEN1 + 1.0: self.t = t
            if t <= self.GAME_LEN2 + 1.0: self.t = max(self.t, t)

        # Update scores & simple momentum
        prev_lead = self.lead
        self.home, self.away = home_score, away_score
        self.lead = float(self.home - self.away)
        if self._seen_event:
            dlead = self.lead - prev_lead
            a = 0.2
            self.momentum = (1.0 - a) * self.momentum + a * dlead
        else:
            self._seen_event = True
            self.momentum = 0.0

        # Hard exits at/near end
        if event_type == "END_GAME":
            self._flatten_all()
            self.reset_state()
            return
        if self.t <= self.CLOSE_OUT_BUFFER:
            self._flatten_all()
            return

        # High-impact detector
        high_impact = False
        if event_type == "SCORE":
            if (shot_type == "THREE_POINT") or (self.t < 30.0):
                high_impact = True
        elif event_type in ("TURNOVER", "STEAL", "FOUL"):
            if self.t < 45.0:
                high_impact = True

        self._try_trade(high_impact)

    def on_orderbook_snapshot(self, ticker: Ticker, bids: list, asks: list) -> None:
        if ticker != Ticker.TEAM_A:
            return
        self._bids.clear(); self._asks.clear()
        for p, q in bids:
            if q >= self.MIN_QTY_LVL:
                self._bids[self._clamp_price(float(p))] = float(q)
        for p, q in asks:
            if q >= self.MIN_QTY_LVL:
                self._asks[self._clamp_price(float(p))] = float(q)
        self._try_trade(False)
