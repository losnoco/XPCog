// The entry point, and the only translation unit that has one -- for the
// reason app/src/main.cpp gives: the library beside this is also linked by the
// tests, and a `main` in a static library is a `main` the linker may take.

#include "GtkApp.hpp"

int main(int argc, char** argv) {
    xpcog::gtk::GtkApp app;
    return app.run(argc, argv);
}
