#if defined _takaro_tf2_positions_included
  #endinput
#endif
#define _takaro_tf2_positions_included

enum struct PositionReceiptState
{
  int pending;
  float sentAt;
  float nextAt;

  void Reset(float now)
  {
    this.pending = 0;
    this.nextAt = now + 3.0;
  }

  bool Ready(float now)
  {
    return this.pending == 0 && now >= this.nextAt;
  }

  void Sent(int seq, float now)
  {
    this.pending = seq;
    this.sentAt = now;
  }

  bool Acknowledge(int seq, float now)
  {
    if (this.pending == 0 || seq != this.pending) return false;
    this.pending = 0;
    this.nextAt = now + 3.0;
    return true;
  }

  bool Expired(float now)
  {
    return this.pending != 0 && now - this.sentAt >= 30.0;
  }
}
