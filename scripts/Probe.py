import unreal

def report(msg):
    unreal.log_warning("[Probe] " + msg)

imc = unreal.load_asset("/Game/Exoneer/Input/IMC_PlayerDefault")
names = [n for n in dir(imc) if ("map" in n.lower()) or ("key" in n.lower())]
report("IMC attrs/methods: " + ", ".join(sorted(names)))

for candidate in ["default_key_mappings", "key_mappings", "default_mappings"]:
    try:
        value = imc.get_editor_property(candidate)
        report("property %s -> %s (%s)" % (candidate, type(value).__name__, str(value)[:200]))
    except Exception as exc:
        report("property %s -> ERROR %s" % (candidate, str(exc)[:120]))

try:
    report("map_key signature test: " + str(type(imc.map_key)))
except Exception as exc:
    report("map_key missing: " + str(exc)[:120])
