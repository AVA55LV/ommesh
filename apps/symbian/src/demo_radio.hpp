#ifndef OMMESH_BELLE_DEMO_RADIO_HPP
#define OMMESH_BELLE_DEMO_RADIO_HPP

class Client;

bool startDemoRadio(Client& client);
void stopDemoRadio(Client& client);
bool demoRadioActive(const Client& client);

#endif
