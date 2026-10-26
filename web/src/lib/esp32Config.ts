// Shared in-memory configuration for ESP32 target IP
let targetIp = '10.38.17.251';

export function getTargetEsp32Ip() {
  return targetIp;
}

export function setTargetEsp32Ip(ip: string) {
  targetIp = ip.trim();
  return targetIp;
}
