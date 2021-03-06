#include <xmpp/adhoc_session.hpp>
#include <xmpp/adhoc_command.hpp>

#include <cassert>

AdhocSession::AdhocSession(const AdhocCommand& command, const std::string& owner_jid,
                           const std::string& to_jid):
  command(command),
  owner_jid(owner_jid),
  to_jid(to_jid),
  current_step(0),
  terminated(false)
{
}

const AdhocStep& AdhocSession::get_next_step()
{
  assert(this->current_step < this->command.callbacks.size());
  return this->command.callbacks[this->current_step++];
}

size_t AdhocSession::remaining_steps() const
{
  return this->command.callbacks.size() - this->current_step;
}

bool AdhocSession::is_terminated() const
{
  return this->terminated;
}

void AdhocSession::terminate()
{
  this->terminated = true;
}
