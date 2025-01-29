#include <sys/socket.h>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <unistd.h>
#include <thread>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <glm/glm.hpp>
#include "json.hpp"

using json = nlohmann::json;

const int PORT = 8080;
const int BUFFER_SIZE = 1024;
int idIndex = 0;

enum BroadcastType
{
  PositionUpdate,
  PlayerLeft,
  PlayerTagged
};

struct PlayerData
{
  glm::vec3 position;
  int serverId;
  bool active = false;
  bool isIt = false;
};

float tagDistance = 3.0f;
int bufferTime = 20;

std::unordered_map<int, PlayerData> clientSockets;
std::unordered_map<int, std::unordered_map<int, glm::vec3>> lastRecvPositions;
std::mutex clientSocketsMutex;
std::mutex tagMutex;

void broadcastTag(const PlayerData &newTagger)
{

  std::lock_guard<std::mutex> lock(clientSocketsMutex);
  json updateMessage = {
      {"type", PlayerTagged},
      {"server-id", newTagger.serverId}};

  std::string jsonString = updateMessage.dump() + "\n";

  for (const auto &client : clientSockets)
  {
    std::cout << "Broadcasting tag to client " << client.first << " : " << jsonString << std::endl;
    if (client.second.isIt)
    {
      json newUpdateMessage = {
          {"type", PlayerTagged},
          {"server-id", -1}};

      std::string newJsonString = newUpdateMessage.dump() + "\n";
      send(client.first, newJsonString.c_str(), newJsonString.length(), 0);
    }
    else
    {
      send(client.first, jsonString.c_str(), jsonString.length(), 0);
    }
  }
}

void checkForTag()
{
  while (true)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(bufferTime));
    bufferTime = 100;

    std::lock_guard<std::mutex> lock(tagMutex);
    bool tagFound = false;

    for (auto &[_, p1] : clientSockets)
    {
      for (auto &[_, p2] : clientSockets)
      {

        if (glm::distance(p1.position, p2.position) <= tagDistance && p1.isIt && !p2.isIt)
        {

          std::cout << "tag, your it!" << std::endl;
          p1.isIt = false;
          p2.isIt = true;
          bufferTime = 3000;
          broadcastTag(p2);
          tagFound = true;
          break;
        }
      }

      if (tagFound)
        break;
    }
  }
}

void broadcastPositionUpdate(const PlayerData &data, int senderSocket)
{
  if (!data.active)
  {
    std::cout << "Client is inactive: " << senderSocket << std::endl;
    return;
  }

  std::lock_guard<std::mutex> lock(clientSocketsMutex);
  json updateMessage = {
      {"type", PositionUpdate},
      {"server-id", data.serverId},
      {"position", {data.position.x, data.position.y, data.position.z}}};

  std::string jsonString = updateMessage.dump() + "\n";

  for (const auto &client : clientSockets)
  {
    if (client.first != senderSocket)
    {
      if (lastRecvPositions[client.first][senderSocket] == data.position)
      {
        continue;
      }
      else
      {
        lastRecvPositions[client.first][senderSocket] = data.position;
      }

      send(client.first, jsonString.c_str(), jsonString.length(), 0);
    }
  }
}

void broadcastCurrentState(int client)
{
  std::lock_guard<std::mutex> lock(clientSocketsMutex);
  for (auto [_, data] : clientSockets)
  {
    if (!data.active)
    {
      continue;
    }

    json updateMessage = {
        {"type", PositionUpdate},
        {"server-id", data.serverId},
        {"position", {data.position.x, data.position.y, data.position.z}}};

    std::string jsonString = updateMessage.dump() + "\n";

    std::cout << "Broadcasting starting value to client " << client << " : " << jsonString << std::endl;

    send(client, jsonString.c_str(), jsonString.length(), 0);
  }

  for (auto [_, data] : clientSockets)
  {
    if (!data.isIt)
    {
      continue;
    }

    json updateMessage = {
        {"type", PlayerTagged},
        {"server-id", data.serverId}};

    std::string jsonString = updateMessage.dump() + "\n";

    std::cout << "Broadcasting starting tag to client " << client << " : " << jsonString << std::endl;

    send(client, jsonString.c_str(), jsonString.length(), 0);
  }
}

void broadcastPlayerLeft(const PlayerData &data, int senderSocket)
{
  if (!data.active)
  {
    std::cout << "Client is inactive: " << senderSocket << std::endl;
    return;
  }

  std::lock_guard<std::mutex> lock(clientSocketsMutex);
  json updateMessage = {
      {"type", PlayerLeft},
      {"server-id", data.serverId}};

  std::string jsonString = updateMessage.dump() + "\n";

  for (const auto &client : clientSockets)
  {
    if (client.first != senderSocket)
    {
      send(client.first, jsonString.c_str(), jsonString.length(), 0);
    }
  }
}

void ensureTagExists()
{
  srand(time(nullptr));
  while (true)
  {
    std::this_thread::sleep_for(std::chrono::seconds(15));

    std::lock_guard<std::mutex> lock(tagMutex);

    if (clientSockets.empty())
    {
      continue;
    }

    bool hasTag = false;
    for (const auto &[_, player] : clientSockets)
    {
      if (player.isIt)
      {
        hasTag = true;
        break;
      }
    }

    if (!hasTag)
    {
      auto it = clientSockets.begin();
      std::advance(it, rand() % clientSockets.size());
      it->second.isIt = true;
      std::cout << "No one was tagged. Assigning player " << it->second.serverId << " as 'It'." << std::endl;

      broadcastTag(it->second);
    }
  }
}

void handleClient(int clientSocket)
{
  char buffer[BUFFER_SIZE];

  broadcastCurrentState(clientSocket);
  {
    std::lock_guard<std::mutex> lock(clientSocketsMutex);
    PlayerData data;
    data.serverId = idIndex++;
    data.position = glm::vec3(0, 0, 0);
    if (clientSockets.size() < 1)
    {
      data.isIt = true;
    }
    clientSockets.emplace(clientSocket, data);
  }

  auto lastFrameTime = std::chrono::high_resolution_clock::now();

  int frameCount = 0;
  while (true)
  {
    auto currentFrameTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<float> delta = currentFrameTime - lastFrameTime;
    float deltaTime = delta.count();
    lastFrameTime = currentFrameTime;

    int bytesRecv = recv(clientSocket, buffer, BUFFER_SIZE - 1, 0);
    if (bytesRecv > 0)
    {
      buffer[bytesRecv] = '\0';

      try
      {
        std::cout << "Received buffer: " << buffer << std::endl;
        json receivedData = json::parse(buffer);
        if (receivedData.contains("position") && receivedData["position"].is_array())
        {
          glm::vec3 newPosition(
              receivedData["position"][0],
              receivedData["position"][1],
              receivedData["position"][2]);

          {
            std::lock_guard<std::mutex> lock(clientSocketsMutex);
            clientSockets[clientSocket].position = newPosition;
            if (!clientSockets[clientSocket].active)
            {
              std::cout << "Set client to active: " << clientSocket << std::endl;
              clientSockets[clientSocket].active = true;
            }
          }

          broadcastPositionUpdate(clientSockets[clientSocket], clientSocket);
        }
        else
        {
          std::cerr << "Invalid recv data: " << receivedData << std::endl;
        }
      }
      catch (json::parse_error &e)
      {
        std::cerr << "JSON parsing error: " << e.what() << std::endl;
      }
    }
    else if (bytesRecv == 0)
    {
      std::cout << "Client disconnected: " << clientSocket << "\n";
      break;
    }
    else
    {
      std::cerr << "Error receiving data from client: " << clientSocket << "\n";
      break;
    }
    frameCount++;
  }

  {
    broadcastPlayerLeft(clientSockets[clientSocket], clientSocket);
    std::lock_guard<std::mutex> lock(clientSocketsMutex);
    clientSockets.erase(clientSocket);
  }

  close(clientSocket);
}

int main()
{
  int server = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr;
  socklen_t addrlen = sizeof(addr);

  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(PORT);

  if (server == -1)
  {
    std::cerr << "Failed to create socket." << std::endl;
    exit(EXIT_FAILURE);
  }

  if (bind(server, (struct sockaddr *)&addr, addrlen) == -1)
  {
    std::cerr << "Failed to bind socket." << std::endl;
    exit(EXIT_FAILURE);
  }

  if (listen(server, 10) == -1)
  {
    std::cerr << "Failed to listen with socket." << std::endl;
    exit(EXIT_FAILURE);
  }

  std::thread tagCheckThread(ensureTagExists);
  tagCheckThread.detach();

  std::thread tagThread(checkForTag);
  tagThread.detach();

  std::cout << "Listening..." << std::endl;

  while (true)
  {
    int client = accept(server, (struct sockaddr *)&addr, &addrlen);
    if (client == -1)
    {
      continue;
    }

    std::cout << "Client joined: " << client << std::endl;

    std::thread clientThread(handleClient, client);
    clientThread.detach();
  }

  close(server);

  return 0;
}
