var device;
var characteristic;

async function connect() {
  console.log("🔍 Searching device...");

  device = await navigator.bluetooth.requestDevice({
    filters: [{ namePrefix: "ESP32" }],
    optionalServices: ["12345678-1234-1234-1234-1234567890ab"],
  });

  console.log("📱 Device selected:", device.name);

  const server = await device.gatt.connect();
  console.log("🔗 GATT connected");

  const service = await server.getPrimaryService(
    "12345678-1234-1234-1234-1234567890ab",
  );
  console.log("📡 Service found");

  characteristic = await service.getCharacteristic(
    "abcd1234-5678-1234-5678-abcdef123456",
  );

  console.log("✅ Characteristic ready");
}

async function sendData() {
  if (!characteristic) {
    console.log("❌ ยังไม่ได้ connect");
    return;
  }

  const value = "Somycs1981_2.4G,0837077299";
  const encoder = new TextEncoder();

  console.log("📤 Sending:", value);

  await characteristic.writeValueWithResponse(encoder.encode(value));

  console.log("✅ Send success");
}
