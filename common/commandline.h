#pragma once

#include <Tempest/Platform>
#include <Tempest/Dir>

#include <cstdint>
#include <stdexcept>
#include <string>

#include "game/constants.h"

class VersionInfo;
class GothicNotFoundException : std::logic_error {
  using logic_error::logic_error;
  };

class CommandLine {
  public:
    CommandLine(int argc,const char** argv);
    static const CommandLine& inst();

    enum GraphicBackend : uint8_t {
      Vulkan,
      DirectX12
      };
    enum class NetMode : uint8_t {
      None,   // singleplayer
      Host,   // -host <port>: listen server
      Client, // -connect <ip:port>
      };
    auto                graphicsApi() const -> GraphicBackend;
    std::u16string_view rootPath() const;
    std::u16string      scriptPath() const;
    std::u16string      scriptPath(ScriptLang lang) const;
    std::u16string      cutscenePath() const;
    std::u16string      cutscenePath(ScriptLang lang) const;
    std::u16string_view modPath() const { return gmod; }
    std::u16string      nestedPath(const std::initializer_list<const char16_t*> &name, Tempest::Dir::FileType type) const;

    bool                isDevMode()        const { return devmode;      }
    bool                isValidationMode() const { return isDebug;      }
    bool                isWindowMode()     const { return isWindow;     }
    bool                isRayQuery()       const { return isRQuery;     }
    GiMethod            isRtGi()           const { return isGi;         }
    bool                isMeshShading()    const { return isMeshSh;     }
    bool                isBindless()       const { return isBindlessSh; }
    bool                isVirtualShadow()  const { return isVsm;        }
    bool                isSoftwareShadow() const { return isRtSm;       }
    bool                doStartMenu()      const { return !noMenu;      }
    Benchmark           isBenchmarkMode()  const { return isBenchmark;  }
    bool                doForceG1()        const { return forceG1;      }
    bool                doForceG2()        const { return forceG2;      }
    bool                doForceG2NR()      const { return forceG2NR;    }
    bool                aaPreset()         const { return aaPresetId;   }
    std::string_view    defaultSave()      const { return saveDef;    }

    NetMode             netMode()          const { return net;          }
    std::string_view    netHost()          const { return netHostName;  } // Client only
    uint16_t            netPort()          const { return netPortNum;   }
    std::string_view    netName()          const { return netPlayer;    }

    std::string         wrldDef;

  private:
    bool                validateGothicPath() const;
    void                setNetMode(NetMode mode, std::string_view flag, const char* value);

    GraphicBackend      graphics = GraphicBackend::Vulkan;
    std::u16string      gpath, gmod;
    std::u16string      gscript;
    std::u16string      gcutscene;
    std::string         saveDef;
    bool                devmode      = false;
    bool                noMenu       = false;
    Benchmark           isBenchmark  = Benchmark::None;
    bool                isWindow     = false;
    bool                isDebug      = false;
#if defined(__OSX__)
    bool                isRQuery     = false;
    bool                isMeshSh     = false;
#else
    bool                isRQuery     = true;
    bool                isMeshSh     = true;
#endif
    bool                isBindlessSh = true;
    bool                isVsm        = false;
    bool                isRtSm       = false;
    GiMethod            isGi         = GiMethod::None;
    bool                forceG1      = false;
    bool                forceG2      = false;
    bool                forceG2NR    = false;
    uint32_t            aaPresetId = 0;
    NetMode             net          = NetMode::None;
    std::string         netHostName;
    uint16_t            netPortNum   = 0;
    std::string         netPlayer    = "Player";
  };

