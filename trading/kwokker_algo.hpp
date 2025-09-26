// ---- Strategy.hpp (drop-in for your template) --------------------------------
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using std::pair;
using std::vector;

class Strategy {
public:
  // ───────── Tunables ─────────
  struct Cfg {
    // Risk / sizing
    static constexpr float MAX_POS = 1200.0f;           // abs contracts
    static constexpr float RISK_PCT_PER_TRADE = 0.0075f;
    static constexpr float POSITION_NUDGE_LATE = 0.25f; // fraction to shed late

    // Microstructure
    static constexpr float MAX_SPREAD_TO_CROSS = 2.0f;  // price points
    static constexpr float PRICE_TICK = 0.1f;
    static constexpr float PASSIVE_IMPROVE = 0.1f;      // improve best by one tick
    static constexpr float MIN_BOOK_QTY = 1.0f;

    // Fair value model
    static constexpr float HOME_ADV_POINTS = 1.25f;
    static constexpr float MOM_EMA_ALPHA = 0.2f;
    static constexpr float BASE_EDGE_THRESH = 0.9f;     // price points
    static constexpr float LATE_TIGHTEN = 0.55f;        // threshold shrink factor late

    // Game/time
    static constexpr float GAME_LEN1 = 2400.0f;
    static constexpr float GAME_LEN2 = 2880.0f;
    static constexpr float INIT_COOLDOWN_SEC = 5.0f;    // after start
    static constexpr float CLOSE_OUT_BUFFER_SEC = 2.0f; // before END_GAME
  };

  // ───────── State ─────────
  struct OB {
    std::map<float, float, std::greater<float>> bids; // price->qty
    std::map<float, float, std::less<float>>    asks; // price->qty
    void clear() { bids.clear(); asks.clear(); }
  } book_;

  float capital_remaining_ = 100000.0f;
  float position_ = 0.0f;

  // Game state
  float t_rem_ = Cfg::GAME_LEN2;
  int   home_ = 0, away_ = 0;
  float lead_ = 0.0f;
  float momentum_ = 0.0f;    // EMA of lead delta
  bool  seen_event_ = false;

  // Control
  bool   inited_ = false;
  double init_wall_ = 0.0;

  // Track one passive order per side so we can cancel/refresh
  std::optional<std::int64_t> working_bid_;
  std::optional<std::int64_t> working_ask_;

  // ───────── Lifecycle ─────────
  void reset_state() {
    book_.clear();
    capital_remaining_ = 100000.0f;
    position_ = 0.0f;

    t_rem_ = Cfg::GAME_LEN2;
    home_ = away_ = 0;
    lead_ = 0.0f;
    momentum_ = 0.0f;
    seen_event_ = false;

    cancel_working_();

    inited_ = true;
    init_wall_ = now_sec_();
  }

  Strategy() { reset_state(); }

  // ───────── Callbacks (exact signatures from your template) ─────────
  void on_trade_update(Ticker /*ticker*/, Side /*side*/, float /*quantity*/, float /*price*/) {
    // Optional: println("trade...");
  }

  void on_orderbook_update(Ticker ticker, Side side, float quantity, float price) {
    if (ticker != Ticker::TEAM_A) return;
    price = clamp_price_(price);

    if (side == Side::buy) {
      if (quantity <= 0.0f) book_.bids.erase(price);
      else                  book_.bids[price] = quantity;
    } else {
      if (quantity <= 0.0f) book_.asks.erase(price);
      else                  book_.asks[price] = quantity;
    }
    try_trade_(/*event_high_impact=*/false);
  }

  void on_account_update(Ticker /*ticker*/, Side /*side*/, float /*price*/, float quantity,
                         float capital_remaining) {
    capital_remaining_ = capital_remaining;
    position_ += quantity; // + for filled buys, - for sells (per engine)
    // If a passive filled, pull the sibling on that side
    if (position_ > 0.0f && working_bid_) { cancel_order(Ticker::TEAM_A, *working_bid_); working_bid_.reset(); }
    if (position_ < 0.0f && working_ask_) { cancel_order(Ticker::TEAM_A, *working_ask_); working_ask_.reset(); }
  }

  virtual void on_game_event_update(
      const std::string& event_type,
      const std::string& /*home_away*/,
      int home_score,
      int away_score,
      const std::optional<std::string>& /*player_name*/,
      const std::optional<std::string>& /*substituted_player_name*/,
      const std::optional<std::string>& shot_type,
      const std::optional<std::string>& /*assist_player*/,
      const std::optional<std::string>& /*rebound_type*/,
      const std::optional<double>& /*coordinate_x*/,
      const std::optional<double>& /*coordinate_y*/,
      const std::optional<double>& time_seconds
  ) {
    if (time_seconds.has_value()) {
      float t = static_cast<float>(*time_seconds);
      if (t <= Cfg::GAME_LEN1 + 1.0f) t_rem_ = t;
      if (t <= Cfg::GAME_LEN2 + 1.0f) t_rem_ = std::max(t_rem_, t);
    }

    // Momentum / score
    float prev_lead = lead_;
    home_ = home_score; away_ = away_score;
    lead_ = float(home_ - away_);
    if (seen_event_) {
      float dlead = lead_ - prev_lead;
      float a = Cfg::MOM_EMA_ALPHA;
      momentum_ = (1.0f - a) * momentum_ + a * dlead;
    } else {
      momentum_ = 0.0f;
      seen_event_ = true;
    }

    // End handling first
    if (event_type == "END_GAME") {
      flatten_all_();
      reset_state();
      return;
    }
    if (t_rem_ <= Cfg::CLOSE_OUT_BUFFER_SEC) {
      flatten_all_();
      return;
    }

    // High-impact detector
    bool high_impact = false;
    if (event_type == "SCORE") {
      if ((shot_type && *shot_type == "THREE_POINT") || t_rem_ < 30.0f) high_impact = true;
    } else if (event_type == "TURNOVER" || event_type == "STEAL" || event_type == "FOUL") {
      if (t_rem_ < 45.0f) high_impact = true;
    }

    try_trade_(high_impact);
  }

  virtual void on_orderbook_snapshot(
      Ticker ticker,
      const vector<pair<float, float>>& bids,
      const vector<pair<float, float>>& asks)
  {
    if (ticker != Ticker::TEAM_A) return;
    book_.clear();
    for (const auto &pq : bids) if (pq.second >= Cfg::MIN_BOOK_QTY) book_.bids[pq.first] = pq.second;
    for (const auto &pq : asks) if (pq.second >= Cfg::MIN_BOOK_QTY) book_.asks[pq.first] = pq.second;
    try_trade_(/*event_high_impact=*/false);
  }

private:
  // ───────── Helpers ─────────
  static double now_sec_() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
  }

  static float clamp_price_(float x) { return std::clamp(x, 0.0f, 100.0f); }

  std::optional<float> best_bid_() const {
    for (auto &kv : book_.bids) if (kv.second >= Cfg::MIN_BOOK_QTY) return kv.first;
    return std::nullopt;
  }
  std::optional<float> best_ask_() const {
    for (auto &kv : book_.asks) if (kv.second >= Cfg::MIN_BOOK_QTY) return kv.first;
    return std::nullopt;
  }
  std::optional<float> mid_() const {
    auto bb = best_bid_(), aa = best_ask_();
    if (bb && aa) return (*bb + *aa) * 0.5f;
    return std::nullopt;
  }

  static float sigmoid_(float x) { return 1.0f / (1.0f + std::exp(-x)); }

  float win_prob_() const {
    float t = std::max(t_rem_, 0.0f);
    float scale = 1.0f / std::sqrt((t / 60.0f) + 1.0f);      // minutes-ish
    float xlead = lead_ * scale;
    float late  = 1.0f + (1.0f - std::tanh(t / 600.0f));     // more weight late
    float xmom  = late * momentum_;
    float xhome = Cfg::HOME_ADV_POINTS * scale;

    float logit = 0.18f * xlead + 0.10f * xmom + 0.20f * xhome;
    return std::clamp(sigmoid_(logit), 0.01f, 0.99f);
  }

  float fair_price_() const { return 100.0f * win_prob_(); }

  float edge_threshold_() const {
    float t = std::max(t_rem_, 0.0f);
    float late_fac = 1.0f - Cfg::LATE_TIGHTEN * (1.0f - std::tanh(t / 600.0f));
    return std::max(0.2f, Cfg::BASE_EDGE_THRESH * late_fac);
  }

  float target_size_for_edge_(float edge, float ref_price) const {
    if (ref_price <= 0.0f) return 0.0f;
    float budget = capital_remaining_ * Cfg::RISK_PCT_PER_TRADE;
    float base   = std::max(1.0f, budget / std::max(1.0f, ref_price)); // contracts
    float t = std::max(t_rem_, 0.0f);
    float urgency = 1.0f + (1.0f - std::tanh(t / 800.0f));             // ~1→2
    float contracts = base * urgency * (0.5f + std::min(1.5f, std::fabs(edge)/2.0f));
    contracts = std::min(contracts, Cfg::MAX_POS * 0.25f);
    return std::max(0.0f, std::floor(contracts));
  }

  void cancel_working_() {
    if (working_bid_) { cancel_order(Ticker::TEAM_A, *working_bid_); working_bid_.reset(); }
    if (working_ask_) { cancel_order(Ticker::TEAM_A, *working_ask_); working_ask_.reset(); }
  }

  void flatten_all_() {
    cancel_working_();
    if (std::fabs(position_) >= 1.0f) {
      if (position_ > 0.0f) place_market_order(Side::sell, Ticker::TEAM_A, std::floor(position_));
      else                  place_market_order(Side::buy,  Ticker::TEAM_A, std::floor(-position_));
    }
  }

  void maybe_place_passives_(float fair, float bestBid, float bestAsk, float midp) {
    float e_buy  = fair - bestAsk;
    float e_sell = bestBid - fair;
    float thr = edge_threshold_();

    if (e_buy > e_sell && e_buy > thr && position_ < Cfg::MAX_POS) {
      float px  = clamp_price_(bestBid + Cfg::PASSIVE_IMPROVE);
      float qty = target_size_for_edge_(e_buy, midp);
      qty = std::min(qty, Cfg::MAX_POS - position_);
      if (qty >= 1.0f) {
        if (working_bid_) cancel_order(Ticker::TEAM_A, *working_bid_);
        if (working_ask_) { cancel_order(Ticker::TEAM_A, *working_ask_); working_ask_.reset(); }
        std::int64_t id = place_limit_order(Side::buy, Ticker::TEAM_A, qty, px, /*ioc=*/false);
        if (id >= 0) working_bid_ = id;
      }
    } else if (e_sell > thr && position_ > -Cfg::MAX_POS) {
      float px  = clamp_price_(bestAsk - Cfg::PASSIVE_IMPROVE);
      float qty = target_size_for_edge_(e_sell, midp);
      qty = std::min(qty, position_ + Cfg::MAX_POS);
      if (qty >= 1.0f) {
        if (working_ask_) cancel_order(Ticker::TEAM_A, *working_ask_);
        if (working_bid_) { cancel_order(Ticker::TEAM_A, *working_bid_); working_bid_.reset(); }
        std::int64_t id = place_limit_order(Side::sell, Ticker::TEAM_A, qty, px, /*ioc=*/false);
        if (id >= 0) working_ask_ = id;
      }
    } else {
      cancel_working_();
    }
  }

  void try_trade_(bool event_high_impact) {
    if (!inited_) return;
    if (now_sec_() - init_wall_ < Cfg::INIT_COOLDOWN_SEC) return;

    auto bb = best_bid_(), aa = best_ask_(), mm = mid_();
    if (!(bb && aa && mm)) return;

    float bestBid = *bb, bestAsk = *aa, midp = *mm;
    float spread = std::max(0.0f, bestAsk - bestBid);
    float fair   = fair_price_();
    float thr    = edge_threshold_();

    float edge_up   = fair - bestAsk; // positive → buy
    float edge_down = bestBid - fair; // positive → sell

    // Late-game inventory nudges
    if (t_rem_ < 60.0f) {
      if (position_ > 0.5f && fair < bestBid) {
        float qty = std::floor(std::max(1.0f, position_ * Cfg::POSITION_NUDGE_LATE));
        place_market_order(Side::sell, Ticker::TEAM_A, qty);
        return;
      } else if (position_ < 0.0f && fair > bestAsk) {
        float qty = std::floor(std::max(1.0f, -position_ * Cfg::POSITION_NUDGE_LATE));
        place_market_order(Side::buy, Ticker::TEAM_A, qty);
        return;
      }
    }

    bool allow_cross = (spread <= Cfg::MAX_SPREAD_TO_CROSS) || event_high_impact;

    if (allow_cross) {
      if (edge_up > thr && position_ < Cfg::MAX_POS) {
        float qty = target_size_for_edge_(edge_up, midp);
        qty = std::min(qty, Cfg::MAX_POS - position_);
        if (qty >= 1.0f) {
          cancel_working_();
          place_limit_order(Side::buy, Ticker::TEAM_A, qty, bestAsk, /*ioc=*/true);
          return;
        }
      }
      if (edge_down > thr && position_ > -Cfg::MAX_POS) {
        float qty = target_size_for_edge_(edge_down, midp);
        qty = std::min(qty, position_ + Cfg::MAX_POS);
        if (qty >= 1.0f) {
          cancel_working_();
          place_limit_order(Side::sell, Ticker::TEAM_A, qty, bestBid, /*ioc=*/true);
          return;
        }
      }
    }

    // Otherwise rest passively
    maybe_place_passives_(fair, bestBid, bestAsk, midp);
  }
};
