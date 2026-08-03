import requests, base64

with open("3.jpg", "rb") as f:
    img_base64 = base64.b64encode(f.read()).decode("utf-8")

url = "https://tanduc2409-esp32-yolo-api.hf.space/api/predict"
response = requests.post(url, json={"data": [img_base64]})

print(response.status_code)
print(response.text)