#include "bondriver/Backend.h"

namespace bondriver {

namespace {

ReceiverTarget makeTarget(const std::string &runtime_dir, const Px4TargetConfig &source)
{
	ReceiverTarget target;
	target.key = "px4:" + runtime_dir + "|" + source.instance + "|" + std::to_string(source.receiver);
	target.label = source.instance + "/" + std::to_string(source.receiver);
	target.instance = source.instance;
	target.receiver = source.receiver;
	target.supports_t = source.supports_t;
	target.supports_s = source.supports_s;
	target.hybrid = source.supports_t && source.supports_s;
	return target;
}

class Px4Backend : public Backend
{
public:
	explicit Px4Backend(const Config &config) : config_(&config)
	{
		if (config_->px4.pool_mode) {
			for (const Px4TargetConfig &source : config_->px4.targets) {
				targets_.push_back(makeTarget(config_->px4.runtime_dir, source));
			}
		} else {
			targets_.push_back(makeTarget(config_->px4.runtime_dir, config_->px4.fixed));
		}
	}

	const std::vector<ReceiverTarget> &targets() const override { return targets_; }

	bool buildCommand(const ChannelEntry &channel, const ReceiverTarget &target,
	                  std::vector<std::string> &argv, std::string &error) const override
	{
		if (target.receiver < 0) {
			error = "PX4 target has no receiver index";
			return false;
		}
		const bool is_s = channel.system == System::IsdbS;
		if (is_s && !target.supports_s) {
			error = "PX4 target does not support isdb-s";
			return false;
		}
		if (!is_s && !target.supports_t) {
			error = "PX4 target does not support isdb-t";
			return false;
		}
		const int64_t khz = (channel.frequency_hz + 500) / 1000;
		argv.clear();
		argv.push_back(config_->common.cli_path);
		argv.push_back("--instance");
		argv.push_back(target.instance);
		argv.push_back("--receiver");
		argv.push_back(std::to_string(target.receiver));
		argv.push_back("--system");
		argv.push_back(is_s ? "isdb-s" : "isdb-t");
		argv.push_back("--frequency-khz");
		argv.push_back(std::to_string(khz));
		argv.push_back("--runtime-dir");
		argv.push_back(config_->px4.runtime_dir);
		argv.push_back("--output");
		argv.push_back("-");
		if (is_s) {
			if (channel.stream_id >= 0) {
				argv.push_back("--stream-id");
				argv.push_back(std::to_string(channel.stream_id));
			} else if (channel.slot >= 0) {
				argv.push_back("--slot");
				argv.push_back(std::to_string(channel.slot));
			} else {
				error = "isdb-s channel has neither stream-id nor slot";
				return false;
			}
			argv.push_back("--lnb-voltage");
			argv.push_back(std::to_string(config_->px4.lnb_voltage));
		}
		return true;
	}

	const char *tag() const override { return "px4"; }

private:
	const Config *config_;
	std::vector<ReceiverTarget> targets_;
};

} // namespace

std::shared_ptr<Backend> createBackend(const Config &config)
{
	return std::make_shared<Px4Backend>(config);
}

} // namespace bondriver
