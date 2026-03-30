let device;
let characteristic;

async function connect() {
  device = await navigator.bluetooth.requestDevice({
    acceptAllDevices: true,
    optionalServices: ["12345678-1234-1234-1234-1234567890ab"],
  });

  const server = await device.gatt.connect();

  const service = await server.getPrimaryService(
    "12345678-1234-1234-1234-1234567890ab",
  );

  characteristic = await service.getCharacteristic(
    "abcd1234-5678-1234-1234-abcdef123456",
  );

  console.log("✅ Connected!");
}

async function sendData(value) {
  if (!characteristic) {
    alert("❌ ยังไม่ได้ connect");
    return;
  }

  const encoder = new TextEncoder();
  await characteristic.writeValue(encoder.encode(value));
}
