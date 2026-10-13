declare module "*.js" {
  const factory: (options?: Record<string, unknown>) => Promise<unknown>;
  export default factory;
}