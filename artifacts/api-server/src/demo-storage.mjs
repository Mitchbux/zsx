import { Storage } from "@google-cloud/storage";
export const storage = new Storage({
  credentials: {
    audience: "replit", subject_token_type: "access_token",
    token_url: "http://127.0.0.1:1106/token", type: "external_account",
    credential_source: { url: "http://127.0.0.1:1106/credential", format: { type: "json", subject_token_field_name: "access_token" } },
    universe_domain: "googleapis.com",
  }, projectId: "",
});
export const demoObject = () => {
  const bucket = process.env.DEFAULT_OBJECT_STORAGE_BUCKET_ID;
  if (!bucket) throw Error("Demo storage is not configured.");
  return storage.bucket(bucket).file("public/zsv/snowboard-full-gray-25fps.zsv");
};
