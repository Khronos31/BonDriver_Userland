#include "bondriver/Backend.h"

namespace bondriver {

namespace {

ReceiverTarget makeTarget(const Config &config, const ReceiverConfig &source)
{
	const ClientConfig *client = config.findClient(source.client);
	ReceiverTarget target;
	target.label = source.client + "/" + source.instance + "/" + std::to_string(source.receiver);
	if (client != nullptr) {
		target.cli_path = client->cli_path;
		target.runtime_dir = client->runtime_dir;
	}
	target.instance = source.instance;
	target.receiver = source.receiver;
	target.supports_t = source.supports_t;
	target.supports_s = source.supports_s;
	target.hybrid = source.hybrid;
	return target;
}

class UserlandBackend : public Backend
{
public:
	explicit UserlandBackend(const Config &config) : config_(&config)
	{
		for (const ReceiverConfig &source : config_->group.receivers) {
			targets_.push_back(makeTarget(*config_, source));
		}
	}

	const std::vector<ReceiverTarget> &targets() const override { return targets_; }

	bool buildCommand(const ChannelEntry &channel, const ReceiverTarget &target,
	                  std::vector<std::string> &argv, std::string &error) const override
	{
		if (target.cli_path.empty()) {
			error = "receiver has no client executable";
			return false;
		}
		if (target.receiver < 0) {
			error = "receiver index is unset";
			return false;
		}
		const bool is_s = channel.system == System::IsdbS;
		if (is_s && !target.supports_s) {
			error = "receiver does not support isdb-s";
			return false;
		}
		if (!is_s && !target.supports_t) {
			error = "receiver does not support isdb-t";
			return false;
		}
		argv.clear();
		argv.push_back(target.cli_path);
		argv.push_back("--instance");
		argv.push_back(target.instance);
		argv.push_back("--receiver");
		argv.push_back(std::to_string(target.receiver));
		argv.push_back("--system");
		argv.push_back(is_s ? "isdb-s" : "isdb-t");
		argv.push_back("--freq");
		argv.push_back(std::to_string(channel.frequency_hz));
		argv.push_back("--runtime-dir");
		argv.push_back(target.runtime_dir);
		argv.push_back("--output");
		argv.push_back("-");
		if (is_s) {
			if (channel.stream_id >= 0) {
				argv.push_back("--tsid");
				argv.push_back(std::to_string(channel.stream_id));
			} else if (channel.slot >= 0) {
				argv.push_back("--slot");
				argv.push_back(std::to_string(channel.slot));
			} else {
				error = "isdb-s channel has neither stream-id nor slot";
				return false;
			}
		}
		return true;
	}

	const char *tag() const override { return "userland"; }

private:
	const Config *config_;
	std::vector<ReceiverTarget> targets_;
};

} // namespace

std::shared_ptr<Backend> createBackend(const Config &config)
{
	return std::make_shared<UserlandBackend>(config);
}

} // namespace bondriver
