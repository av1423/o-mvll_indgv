import omvll
from functools import lru_cache

class MyConfig(omvll.ObfuscationConfig):
    def __init__(self):
        super().__init__()

    # 100%
    def indirect_global_variable(self, module, global_var):
        return omvll.IndirectGlobalVariableWithProbability(100)

@lru_cache(maxsize=1)
def omvll_get_config() -> omvll.ObfuscationConfig:
    return MyConfig()
