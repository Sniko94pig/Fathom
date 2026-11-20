package fathom.mc;

import net.fabricmc.api.ClientModInitializer;

public final class FathomMod implements ClientModInitializer {
    @Override
    public void onInitializeClient() {
        Link.start();
    }
}
