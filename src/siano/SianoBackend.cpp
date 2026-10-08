#include "bondriver/Backend.h"

namespace bondriver {

namespace {

class SianoBackend : public Backend
{
public:
	explicit SianoBackend(const Config &config) : config_(&config)
	{
		if (config_->siano.pool_mode) {
			for (const std::string &device : config_->siano.devices) {
				targets_.push_back(makeTarget(device));
			}
		} else {
			targets_.push_back(makeTarget(config_->siano.device));
		}
	}

	const std::vector<ReceiverTarget> &targets() const override { return targets_; }

	bool buildCommand(const ChannelEntry &channel, const ReceiverTarget &target,
	                  std::vector<std::string> &argv, std::string &error) const override
	{
		if (channel.system != System::IsdbT) {
			error = "Siano backend supports isdb-t only";
			return false;
		}
		if (target.selector.empty()) {
			error = "empty Siano selector";
			return false;
		}
		argv.clear();
		argv.push_back(config_->common.cli_path);
		argv.push_back("--device");
		argv.push_back(target.selector);
		if (!config_->siano.firmware.empty()) {
			argv.push_back("--firmware");
			argv.push_back(config_->siano.firmware);
		}
		argv.push_back("--freq");
		argv.push_back(std::to_string(channel.frequency_hz));
		return true;
	}

	const char *tag() const override { return "siano"; }

private:
	static ReceiverTarget makeTarget(const std::string &selector)
	{
		ReceiverTarget target;
		target.key = "siano:" + selector;
		target.label = selector;
		target.selector = selector;
		target.supports_t = true;
		target.supports_s = false;
		target.hybrid = false;
		return target;
	}

	const Config *config_;
	std::vector<ReceiverTarget> targets_;
};

} // namespace

std::shared_ptr<Backend> createBackend(const Config &config)
{
	return std::make_shared<SianoBackend>(config);
}

} // namespace bondriver
