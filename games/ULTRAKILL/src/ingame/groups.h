#pragma once
#include "ingame/cheats.h"
#include "ingame/log.h"
#include <string>

namespace ukplayer {

bool Resolve();
bool Supported(const std::string& id);
void Tick(const uk::Ctx& c);
void Release(const uk::Ctx& c);
void Action(const std::string& id, const uk::Ctx& c);
bool Busy();

}

namespace ukmove {

bool Resolve();
bool Supported(const std::string& id);
void Tick(const uk::Ctx& c);
void Release(const uk::Ctx& c);
void Action(const std::string& id, const uk::Ctx& c);
bool Busy();

}

namespace ukweapa {

bool Resolve();
bool Supported(const std::string& id);
void Tick(const uk::Ctx& c);
void Release(const uk::Ctx& c);
void Action(const std::string& id, const uk::Ctx& c);
bool Busy();

}

namespace ukweapb {

bool Resolve();
bool Supported(const std::string& id);
void Tick(const uk::Ctx& c);
void Release(const uk::Ctx& c);
void Action(const std::string& id, const uk::Ctx& c);
bool Busy();

}

namespace ukshop {

bool Resolve();
bool Supported(const std::string& id);
void Tick(const uk::Ctx& c);
void Release(const uk::Ctx& c);
void Action(const std::string& id, const uk::Ctx& c);
bool Busy();

}

namespace ukaim {

bool Resolve();
bool Supported(const std::string& id);
void Tick(const uk::Ctx& c);
void Release(const uk::Ctx& c);
void Action(const std::string& id, const uk::Ctx& c);
bool Busy();

}

namespace ukorbs {

bool Resolve();
bool Supported(const std::string& id);
void Tick(const uk::Ctx& c);
void Release(const uk::Ctx& c);
void Action(const std::string& id, const uk::Ctx& c);
bool Busy();

}

namespace uklevel {

bool Resolve();
bool Supported(const std::string& id);
void Tick(const uk::Ctx& c);
void Release(const uk::Ctx& c);
void Action(const std::string& id, const uk::Ctx& c);
bool Busy();
bool Outside();

}
